
#include <netp/core.hpp>
#include <netp/app.hpp>

#ifdef  __cplusplus
	extern "C" {
#endif
	#include "../3rd/c-ares/c-ares-1.18.1/src/lib/ares_writev.h"
	#include "../3rd/c-ares/c-ares-1.18.1/src/lib/ares_private.h"
#ifdef  __cplusplus
	}
#endif 

#include "../3rd/c-ares/c-ares-1.18.1/include/ares.h"


#include <netp/dns_resolver.hpp>
#include <netp/socket_api.hpp>
#include <netp/event_loop.hpp>

namespace netp {

	const static int dns_error_map[] = {
		0,
		netp::E_DNS_TEMPORARY_ERROR,
		netp::E_DNS_PROTOCOL_ERROR,
		netp::E_DNS_DOMAIN_NAME_NOT_EXISTS,
		netp::E_DNS_DOMAIN_NO_DATA,
		netp::E_DNS_NOMEM,
		netp::E_DNS_BADQUERY
	};

	ares_fd_monitor::ares_fd_monitor(dns_resolver& _dnsr_, SOCKET fd_) :
		dnsr(_dnsr_),
		flag(0),
		fd(fd_),
		ctx(0)
	{}

	void ares_fd_monitor::io_end() {
		if (flag & f_ares_fd_closed) { return; }
		flag |= f_ares_fd_closed;

		if (flag & f_ares_fd_watch_read) {
			flag &= ~f_ares_fd_watch_read;
			//epoll_ctl(del) shold always happens before close(fd)
			dnsr.L->io_do(io_action::END_READ, ctx);
		}

		if (flag & f_ares_fd_watch_write) {
			flag &= ~f_ares_fd_watch_write;
			//epoll_ctl(del) shold always happens before close(fd)
			dnsr.L->io_do(io_action::END_WRITE, ctx);
		}
	}

	void ares_fd_monitor::io_notify_terminating(int, io_ctx*) {
		//fake a shutdown operation first, let read return error
		netp::shutdown(fd, SHUT_RD);
		ares_process_fd(*((ares_channel*)(dnsr.m_ares_channel)), fd, ARES_SOCKET_BAD);
	}

	void ares_fd_monitor::io_notify_read(int status, io_ctx*) {
		if (status == netp::OK) {
			ares_process_fd(*((ares_channel*)(dnsr.m_ares_channel)), fd, ARES_SOCKET_BAD);
		} else {
			netp::shutdown(fd, SHUT_RD);
			ares_process_fd(*((ares_channel*)(dnsr.m_ares_channel)), fd, ARES_SOCKET_BAD);
		}
	}
	void ares_fd_monitor::io_notify_write(int status, io_ctx*) {
		if (status == netp::OK) {
			ares_process_fd(*((ares_channel*)(dnsr.m_ares_channel)), ARES_SOCKET_BAD, fd);
		} else {
			netp::shutdown(fd, SHUT_RD);
			ares_process_fd(*((ares_channel*)(dnsr.m_ares_channel)), fd, ARES_SOCKET_BAD);
		}
	}

	void dns_resolver::__ares_check_timeout() {
		NETP_ASSERT(m_flag & f_running);

		if ((m_ares_active_query > 0) && ((m_flag & f_timeout_timer) == 0)) {
			m_flag |= f_timeout_timer;
			NRP<netp::promise<int>> ltp = netp::make_ref<netp::promise<int>>();
			ltp->if_done([dnsr=NRP<dns_resolver>(this)](int rt) {
				NETP_ASSERT( dnsr->L->in_event_loop() );
				if (rt != netp::OK) {
					dnsr->m_flag &= ~f_timeout_timer;
				}
			});

			struct timeval nxt_exp = { 1,1 };
			NETP_ASSERT(m_tm_dnstimeout != nullptr);
			ares_timeout(*((ares_channel*)(m_ares_channel)), 0, &nxt_exp);
			m_tm_dnstimeout->set_delay(std::chrono::milliseconds((nxt_exp.tv_sec * 1000) + (nxt_exp.tv_usec / 1000)));
			L->launch(m_tm_dnstimeout, ltp);
		}
	}

	dns_resolver::dns_resolver(NRP<event_loop> const& L_) :
		L(L_),
		m_ares_channel(0),
		m_ares_active_query(0),
		m_flag(0)
	{
		NETP_ASSERT(L_ != nullptr );
	}

	dns_resolver::~dns_resolver() {}

	void dns_resolver::init() {
		NETP_ASSERT(m_ares_channel ==0);
		m_ares_channel = netp::allocator<ares_channel>::make();
	}
	void dns_resolver::deinit() {
		if (m_ares_channel != 0) {
			netp::allocator<ares_channel>::trash((ares_channel*)m_ares_channel);
			m_ares_channel = 0;
		}
	}

	void dns_resolver::_do_add_name_server() {
		NETP_ASSERT(L->in_event_loop());
		std::for_each(m_ns.begin(), m_ns.end(), [&](netp::string_t const& serv) {
			NETP_VERBOSE("[dns_resolver]add dns serv: %s", serv.c_str());
		});
	}

	NRP<netp::promise<int>> dns_resolver::add_name_server(std::vector<netp::string_t, netp::allocator<netp::string_t>> const& ns) {
		NRP<netp::promise<int>> p = netp::make_ref<netp::promise<int>>();
		L->execute([dnsr=NRP<dns_resolver>(this), ns, p]() {
			dnsr->m_ns.insert(dnsr->m_ns.begin(), ns.begin(), ns.end());
			p->set(netp::OK);
		});
		return p;
	}

	static void ___ares_socket_state_cb(void* data, ares_socket_t socket_fd, int readable, int writable) {
		NETP_ASSERT(data != 0);
		dns_resolver* dnsr = (dns_resolver*)data;
		dnsr->__ares_socket_state_cb(socket_fd, readable, writable);
	}
	/*
	static int ___ares_socket_create_cb(ares_socket_t socket_fd, int type, void* data) {
		NETP_ASSERT(data != 0);
		dns_resolver* dnsr = (dns_resolver*)data;
		return dnsr->__ares_socket_create_cb(socket_fd, type);
	}
	*/

	static ares_socket_t ___ares_socket_create(int af, int type, int proto, void* data) {
		dns_resolver* dnsr = (dns_resolver*)data;
		return dnsr->__ares_socket_create(af,type,proto);
	}

	static int ___ares_socket_close(ares_socket_t fd, void* data) {
		dns_resolver* dnsr = (dns_resolver*)data;
		return dnsr->__ares_socket_close(fd);
	}

	static int ___ares_socket_connect(ares_socket_t fd, const struct sockaddr* sockaddr_, ares_socklen_t len, void*) {
		return ::connect(fd, sockaddr_, len);
	}
	static ares_ssize_t ___ares_socket_recvfrom(ares_socket_t fd, void* buf, size_t len, int flag, struct sockaddr* from, ares_socklen_t* addrlen, void*) {
		return ::recvfrom(fd, (char*)buf, len, flag, from, addrlen);
	}
	static ares_ssize_t ___ares_socket_sendv(ares_socket_t fd, const struct iovec* iovec_, int count, void*) {
		return writev(fd,iovec_,count);
	}

	static const ares_socket_functions __ares_func = {
		___ares_socket_create,
		___ares_socket_close,
		___ares_socket_connect,
		___ares_socket_recvfrom,
		___ares_socket_sendv
	};

	void dns_resolver::_do_start(NRP<netp::promise<int>> const& p) {
		NETP_ASSERT(L->in_event_loop());

		if ( m_flag &( u8_t(dns_resolver_flag::f_launching|dns_resolver_flag::f_running|dns_resolver_flag::f_stop_called)) ) {
			p->set(netp::E_DNS_INVALID_STATE);
			return;
		}

		m_flag |= dns_resolver_flag::f_launching;
		int ares_init_rt = ares_library_init(ARES_LIB_INIT_ALL);
		if (ares_init_rt != ARES_SUCCESS) {
			m_flag &= ~dns_resolver_flag::f_launching;
			p->set(ares_init_rt);
			return;
		}
		ares_options ares_opt;
		std::memset(&ares_opt, 0, sizeof(ares_opt));

		int ares_flag = ARES_OPT_SOCK_STATE_CB;
		ares_opt.sock_state_cb = ___ares_socket_state_cb;
		ares_opt.sock_state_cb_data = this;

		ares_flag |= ARES_OPT_FLAGS;
		ares_opt.flags = ARES_FLAG_STAYOPEN;
		
		ares_init_rt = ares_init_options(((ares_channel*)(m_ares_channel)), &ares_opt, ares_flag);
		if (ares_init_rt != ARES_SUCCESS) {
			m_flag &= ~dns_resolver_flag::f_launching;
			p->set(ares_init_rt);
			return;
		}

		ares_set_socket_functions(*((ares_channel*)(m_ares_channel)), &__ares_func, this );

		//@note
		//it's safe to set this pointer, cuz ares ares_destroy always happens before loop exit
		//for def loop, it's always true
		//for non-def-loop, if it exit before def loop's exit, it's always true
		// 
		//ares_set_socket_callback(m_ares_channel, ___ares_socket_create_cb, this);

		ares_addr_node* ns;
		int read_server_rt = ares_get_servers(*((ares_channel*)(m_ares_channel)), &ns);
		NETP_ASSERT(read_server_rt == ARES_SUCCESS);
		for (ares_addr_node* n = ns; NULL!=n; n = n->next) {
			switch (n->family) {
			case AF_INET:
			{
				NETP_INFO("ns: %s", nipv4todotip({ n->addr.addr4.s_addr }).c_str());
			}
			break;
			case AF_INET6:
			{
				//NETP_TODO("print ns");
			}
			break;
			default:
			{}
			break;
			}
		}
		ares_free_data(ns);

		NETP_ASSERT(m_tm_dnstimeout == nullptr);
		m_tm_dnstimeout = netp::make_ref<netp::timer>(std::chrono::milliseconds(100), &dns_resolver::cb_dns_timeout, NRP<dns_resolver>(this), std::placeholders::_1);

		m_flag &= ~dns_resolver_flag::f_launching;
		m_flag |= dns_resolver_flag::f_running;

		NETP_INFO("[dns_resolver]launched");
		p->set(netp::OK);
	}

	NRP<netp::promise<int>> dns_resolver::start() {
		NETP_ASSERT( L != nullptr );
		NRP<netp::promise<int>> p = netp::make_ref<netp::promise<int>>();
		L->execute([dnsr=NRP<dns_resolver>(this),p]() {
			dnsr->_do_start(p);
		});
		return p;
	}

	void dns_resolver::_do_stop(NRP<netp::promise<int>> const& p) {
		NETP_ASSERT(L->in_event_loop());
		if ((m_flag& dns_resolver_flag::f_running) == 0) {
			p->set(netp::E_DNS_INVALID_STATE);
			return;
		}
		m_flag &= ~dns_resolver_flag::f_running;

		ares_cancel(*((ares_channel*)(m_ares_channel)));
		ares_destroy(*((ares_channel*)(m_ares_channel)));

		__ares_wait();

		ares_library_cleanup();

		NETP_ASSERT(m_tm_dnstimeout != nullptr);
		m_tm_dnstimeout = nullptr;

		NETP_INFO("[dns_resolver]stoped");
		p->set(netp::OK);
	}

	void dns_resolver::restart() {
		NETP_ASSERT( L->in_event_loop() );
		if ((m_flag&dns_resolver_flag::f_restarting)) {
			return;
		}

		if (m_flag&dns_resolver_flag::f_timeout_barrier) {
			m_flag |= f_restarting_pending;
			return;
		}

		m_flag |= dns_resolver_flag::f_restarting;
		NRP<netp::promise<int>> p_stop = netp::make_ref<netp::promise<int>>();
		_do_stop(p_stop);

		NRP<netp::promise<int>> p_start = netp::make_ref<netp::promise<int>>();
		_do_start(p_start);
		m_flag &= ~dns_resolver_flag::f_restarting;
	}

	NRP<netp::promise<int>> dns_resolver::stop() {
		NRP<netp::promise<int>> p = netp::make_ref<netp::promise<int>>();
		L->execute([dnsr=NRP<dns_resolver>(this),p]() {
			dnsr->m_flag |= f_stop_called;
			dnsr->_do_stop(p);
		});
		return p;
	}

	void dns_resolver::cb_dns_timeout(NRP<netp::timer> const&) {
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(m_flag&f_timeout_timer);
		m_flag &= ~f_timeout_timer;
		if ( (m_flag& dns_resolver_flag::f_running) == 0  || (m_ares_active_query ==0) ) {
			return;
		}

		//ares_process_fd might result in insert/erase pair from m_ares_fd_monitor_map
		//ares_process_fd might result in restart

		m_flag |= f_timeout_barrier;
		ares_process_fd(*((ares_channel*)m_ares_channel), ARES_SOCKET_BAD, ARES_SOCKET_BAD);
		m_flag &= ~f_timeout_barrier;

		if (m_flag & f_restarting_pending) {
			m_flag &= ~f_restarting_pending;
			restart();
			return;
		}

		__ares_check_timeout();
	}

	void dns_resolver::__ares_wait() {
		NETP_ASSERT(m_ares_fd_monitor_map.size() == 0);
		NETP_ASSERT(m_ares_active_query == 0);
	}

	SOCKET dns_resolver::__ares_socket_create(int af, int type, int proto) {
		NETP_ASSERT(L != nullptr && L->in_event_loop());

		SOCKET fd = socket(af, type, proto);
		NETP_RETURN_V_IF_MATCH(fd, fd == NETP_INVALID_SOCKET);
		
		int setnb = netp::set_nonblocking(fd, true);
		if (setnb != netp::OK) {
			netp::close(fd);
			return NETP_INVALID_SOCKET;
		}
		
		NRP<netp::ares_fd_monitor> afm = netp::make_ref<netp::ares_fd_monitor>(*this, fd);
		io_ctx* ctx = L->io_begin(fd, afm);
		if (ctx == nullptr) {
			netp::close(fd);
			return NETP_INVALID_SOCKET;
		}

		afm->ctx = ctx;
		m_ares_fd_monitor_map.insert({ fd, afm });
		return fd;
	}

	int dns_resolver::__ares_socket_close(SOCKET fd) {
		NETP_ASSERT(L != nullptr);
		NETP_ASSERT(L->in_event_loop());
		ares_fd_monitor_map_t::iterator it = m_ares_fd_monitor_map.find(fd);
		if (it != m_ares_fd_monitor_map.end()) {
			it->second->io_end();
			netp::close(fd);
			L->schedule([afm = it->second, L_=L, ctx_ = it->second->ctx]() {
				L_->io_end(ctx_);
			});
			m_ares_fd_monitor_map.erase(fd);
		}
		return netp::OK;
	}

	/*
	int dns_resolver::__ares_socket_create_cb(ares_socket_t socket_fd, int type) {
		(void)type;
		//NETP_VERBOSE("[dns_resolver]__ares_socket_create_cb, fd: %d, type: %d", socket_fd, type);
		NETP_ASSERT(L != nullptr);
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT((m_flag & dns_resolver_flag::f_running));
		ares_fd_monitor_map_t::iterator it = m_ares_fd_monitor_map.find(socket_fd);
		NETP_ASSERT(it != m_ares_fd_monitor_map.end());
	}
	*/

	void dns_resolver::__ares_socket_state_cb(SOCKET socket_fd, int readable, int writable) {
		//NETP_VERBOSE("[dns_resolver]__ares_state_cb, fd: %d, readable: %d, writeable: %d", socket_fd, readable, writable);

		//if ((m_flag & dns_resolver_flag::f_running) ==0) { return; }
		ares_fd_monitor_map_t::iterator it = m_ares_fd_monitor_map.find(socket_fd);
		//BOTH READ|WRITE would trigger erase
		if (it == m_ares_fd_monitor_map.end()) { return; }

		NRP<ares_fd_monitor> m = it->second;
		if (readable == 1 && (m->flag&f_ares_fd_watch_read) ==0) {
			int rt = L->io_do(io_action::READ, m->ctx);
			if (rt != netp::OK) {
				netp::shutdown(socket_fd, SHUT_RD);
				ares_process_fd(*((ares_channel*)m_ares_channel), socket_fd, ARES_SOCKET_BAD);
			} else {
				m->flag |= f_ares_fd_watch_read;
			}
		} else if(readable == 0 && (m->flag&f_ares_fd_watch_read) !=0 ){
			int rt = L->io_do(io_action::END_READ, m->ctx);
			m->flag &= ~f_ares_fd_watch_read;
			if (rt != netp::OK) {
				netp::shutdown(socket_fd, SHUT_RD);
				ares_process_fd(*((ares_channel*)m_ares_channel), socket_fd, ARES_SOCKET_BAD);
			}
		}

		if (writable == 1 && (m->flag&f_ares_fd_watch_write) == 0) {
			int rt = L->io_do(io_action::WRITE, m->ctx);
			if (rt != netp::OK) {
				//fake a read error
				netp::shutdown(socket_fd, SHUT_RD);
				ares_process_fd(*((ares_channel*)m_ares_channel), socket_fd, ARES_SOCKET_BAD);
			} else {
				m->flag |= f_ares_fd_watch_write;
			}
		} else if(writable == 0 && (m->flag&f_ares_fd_watch_write) !=0 ){
			int rt = L->io_do(io_action::END_WRITE, m->ctx);
			m->flag &= ~f_ares_fd_watch_write;
			if (rt != netp::OK) {
				netp::shutdown(socket_fd, SHUT_RD);
				ares_process_fd(*((ares_channel*)m_ares_channel), socket_fd, ARES_SOCKET_BAD);
			}
		}

		if ( (readable == 0 && writable == 0) && (m->flag&(f_ares_fd_watch_read|f_ares_fd_watch_write)) == 0 ) {
			__ares_socket_close(socket_fd);
		}
	}

	void dns_resolver::__ares_gethostbyname_cb(void* arg, int status, int timeouts, struct hostent* hostent)
	{
		(void)timeouts;
		async_dns_query* adq = (async_dns_query*)arg;
#ifdef _NETP_DEBUG
		NETP_ASSERT(adq->dnsr->L->in_event_loop());
#endif
		adq->dnsr->__ares_done();
		if (status == ARES_SUCCESS) {
			std::vector<netp::ipv4_t, netp::allocator<netp::ipv4_t>> ipv4s;
			char** lpSrc;
			for (lpSrc = hostent->h_addr_list; *lpSrc; lpSrc++) {
				switch (hostent->h_addrtype) {
				case AF_INET:
				{
					char addr_buf[32] = {0};
					ares_inet_ntop(hostent->h_addrtype, *lpSrc, addr_buf, sizeof(addr_buf));
					ipv4s.emplace_back( dotiptoip(addr_buf));
					//u_long nip = u32_t(*lpSrc);
					//ipv4s.push_back( ntohl(nip) );
				}
				break;
				case AF_INET6:
				{
					NETP_TODO("AF_INET6");
				}
				break;
				}
			}

			if (ipv4s.size()) {
				adq->dnsquery_p->set(std::make_tuple(netp::OK, ipv4s));
			} else {
				adq->dnsquery_p->set(std::make_tuple(netp::E_DNS_DOMAIN_NO_DATA, ipv4s));
			}
		} else {
#ifdef _NETP_DEBUG
			NETP_WARN("[dns_resolver]resolve status: %d, host: %s", status, adq->host.c_str() );
#else
			NETP_WARN("[dns_resolver]resolve status: %d", status);
#endif
			adq->dnsquery_p->set(std::make_tuple( NETP_NEGATIVE(NETP_ABS(netp::E_DNS_CARES_ERRNO_BEGIN)+NETP_ABS(status)), std::vector<netp::ipv4_t, netp::allocator<netp::ipv4_t>>()));
		}
		if (status == ARES_ECONNREFUSED) {
			NETP_WARN("[dns_resolver]resolve status: %d", ARES_ECONNREFUSED);
			adq->dnsr->L->schedule([dnsr=adq->dnsr]() {
				dnsr->restart();
			});
		}

		netp::allocator<async_dns_query>::trash(adq);
	}

	void dns_resolver::_do_resolve(string_t const& domain, NRP<dns_query_promise> const& p) {
		NETP_ASSERT(L->in_event_loop());
		if ( (m_flag& dns_resolver_flag::f_running) == 0) {
			p->set(std::make_tuple(netp::E_DNS_INVALID_STATE,std::vector<ipv4_t,netp::allocator<ipv4_t>>()));
			return;
		}

		async_dns_query* adq = netp::allocator<async_dns_query>::make();
		adq->dnsr = NRP<dns_resolver>(this);
		adq->dnsquery_p = p;
		#ifdef _NETP_DEBUG
		adq->host = netp::string_t(domain.c_str(), domain.length());
		#endif

		++m_ares_active_query;
		ares_gethostbyname(*((ares_channel*)m_ares_channel), domain.c_str(), AF_INET, __ares_gethostbyname_cb, adq);
		__ares_check_timeout();
	}

	NRP<dns_query_promise> dns_resolver::resolve(string_t const& domain) {
		NRP<dns_query_promise> dnsp = netp::make_ref<dns_query_promise>();
		L->execute([dnsr=NRP<dns_resolver>(this), domain, dnsp]() {
			dnsr->_do_resolve(domain, dnsp);
		});
		return dnsp;
	}
}