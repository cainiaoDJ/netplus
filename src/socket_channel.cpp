#include <netp/core.hpp>
#include <netp/logger_broker.hpp>
#include <netp/socket.hpp>

namespace netp {

	int socket_channel::connect(address const& addr) {
		if (m_chflag & (int(channel_flag::F_CONNECTING) | int(channel_flag::F_CONNECTED) | int(channel_flag::F_LISTENING) | int(channel_flag::F_CLOSED)) ) {
			return netp::E_SOCKET_INVALID_STATE;
		}
		NETP_ASSERT((m_chflag & int(channel_flag::F_ACTIVE)) == 0);
		channel::ch_set_active();
		int rt= socket_base::connect(addr);
		if (rt == netp::OK) {
			m_chflag |= int(channel_flag::F_CONNECTED);
			return netp::OK;
		} else if (IS_ERRNO_EQUAL_CONNECTING(rt)) {
			m_chflag |= int(channel_flag::F_CONNECTING);
		} else {
			ch_errno() = rt;
		}
		return rt;
	}

	void socket_channel::_tmcb_BDL(NRP<timer> const& t) {
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(m_outbound_limit > 0);
		NETP_ASSERT(m_chflag&int(channel_flag::F_BDLIMIT_TIMER) );
		m_chflag &= ~int(channel_flag::F_BDLIMIT_TIMER);
		if (m_chflag & (int(channel_flag::F_WRITE_SHUTDOWN)|int(channel_flag::F_WRITE_ERROR)|int(channel_flag::F_IO_EVENT_LOOP_NOTIFY_TERMINATING))) {
			return;
		}

		NETP_ASSERT(m_outbound_limit>=m_outbound_budget);
		u32_t tokens = m_outbound_limit / (1000/NETP_SOCKET_BDLIMIT_TIMER_DELAY_DUR);
		if ( m_outbound_limit < (tokens+ m_outbound_budget)) {
			m_outbound_budget = m_outbound_limit;
		} else {
			m_chflag |= int(channel_flag::F_BDLIMIT_TIMER);
			m_outbound_budget += tokens;
			L->launch(t);
		}

		if (m_chflag & int(channel_flag::F_BDLIMIT)) {
			NETP_ASSERT( !(m_chflag & (int(channel_flag::F_WRITE_BARRIER)|int(channel_flag::F_WATCH_WRITE))));
			m_chflag &= ~int(channel_flag::F_BDLIMIT);
			m_chflag |= int(channel_flag::F_WRITE_BARRIER);
			__cb_io_write_impl(netp::OK, m_io_ctx);
			m_chflag &= ~int(channel_flag::F_WRITE_BARRIER);
		}
	}


	int socket_channel::bind(netp::address const& addr) {
		if (m_chflag & int(channel_flag::F_CLOSED)) {
			return netp::E_SOCKET_INVALID_STATE;
		}
		int rt = socket_base::bind(addr);
		NETP_TRACE_SOCKET("[socket][%s]socket bind rt: %d", ch_info().c_str(), rt );
		return rt;
	}

	int socket_channel::listen( int backlog ) {
		NETP_ASSERT(L->in_event_loop());
		if (m_chflag & int(channel_flag::F_ACTIVE)) {
			return netp::E_SOCKET_INVALID_STATE;
		}

		NETP_ASSERT((m_fd > 0)&&(m_chflag & int(channel_flag::F_LISTENING)) == 0);
		m_chflag |= int(channel_flag::F_LISTENING);
		int rt = socket_base::listen( backlog);
		NETP_TRACE_SOCKET("[socket][%s]socket listen rt: %d", ch_info().c_str(), rt);
		return rt;
	}

	void socket_channel::do_listen_on(address const& addr, fn_channel_initializer_t const& fn_accepted_initializer, NRP<promise<int>> const& chp, NRP<socket_cfg> const& ccfg, int backlog ) {
		if (!L->in_event_loop()) {
			L->schedule([_this=NRP<socket_channel>(this), addr, fn_accepted_initializer, chp,ccfg, backlog]() ->void {
				_this->do_listen_on(addr, fn_accepted_initializer, chp, ccfg, backlog);
			});
			return;
		}

		//int rt = -10043;
		int rt = socket_channel::bind(addr);
		if (rt != netp::OK) {
			m_chflag |= int(channel_flag::F_READ_ERROR);//for assert check
			NETP_WARN("[socket]socket::bind(): %d, addr: %s", rt, addr.to_string().c_str() );
			chp->set(rt);
			return;
		}

		rt = socket_channel::listen(backlog);
		if (rt != netp::OK) {
			m_chflag |= int(channel_flag::F_READ_ERROR);//for assert check
			NETP_WARN("[socket]socket::listen(%u): %d, addr: %s",backlog, rt, addr.to_string().c_str());
			chp->set(rt);
			return;
		}

		NETP_ASSERT(rt == netp::OK);
		ccfg->family = m_family;
		ccfg->type = m_type;
		ccfg->proto = m_protocol;

		ch_io_begin([fn_accepted_initializer,chp, ccfg, so = NRP<socket_channel>(this)]( int status, io_ctx*){
			chp->set(status);
			if(status == netp::OK) {
				auto fn_accept = std::bind(&socket_channel::__cb_io_accept_impl, so, ccfg, fn_accepted_initializer, std::placeholders::_1, std::placeholders::_2);
				so->ch_io_accept(fn_accept);
			}
		});
	}

	void socket_channel::do_dial(address const& addr, fn_channel_initializer_t const& fn_initializer, NRP<promise<int>> const& dialp) {
		NETP_ASSERT(L->in_event_loop());
		ch_io_begin([so=NRP<socket_channel>(this),addr, fn_initializer, dialp](int status, io_ctx*) {
			NETP_ASSERT(so->L->in_event_loop());
			if (status != netp::OK) {
				dialp->set(status);
				return;
			}
			int rt = so->connect(addr);
			if (rt == netp::OK) {
				NETP_TRACE_SOCKET("[socket][%s]socket connected directly", so->ch_info().c_str());
				so->_ch_do_dial_done_impl(fn_initializer, dialp, netp::OK, so->m_io_ctx);
				return;
			} else if (IS_ERRNO_EQUAL_CONNECTING(rt)) {
				auto fn_connect_done = std::bind(&socket_channel::_ch_do_dial_done_impl, so, fn_initializer, dialp, std::placeholders::_1, std::placeholders::_2);
				so->ch_io_connect(fn_connect_done);
				return;
			}
			dialp->set(rt);
		});
	}

	SOCKET socket_channel::accept( address& raddr ) {
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(m_chflag & int(channel_flag::F_LISTENING));
		if ( (m_chflag & int(channel_flag::F_LISTENING)) ==0 ) {
			netp_set_last_errno(netp::E_SOCKET_INVALID_STATE);
			return (SOCKET)NETP_SOCKET_ERROR;
		}

		return socket_base::accept(raddr);
	}

	void socket_channel::_ch_do_dial_done_impl(fn_channel_initializer_t const& fn_initializer, NRP<promise<int>> const& dialp_, int status, io_ctx* ctx) {
		NETP_ASSERT(L->in_event_loop());
		NRP<promise<int>> dialp = dialp_;

		if (status != netp::OK) {
		_set_fail_and_return:
			NETP_ASSERT(status != netp::OK);
			m_chflag |= int(channel_flag::F_WRITE_ERROR);
			ch_io_end_connect();
			NETP_ERR("[socket][%s]socket dial error: %d", ch_info().c_str(), status);
			dialp->set(status);
			return;
		}

		if( m_chflag& int(channel_flag::F_CLOSED)) {
			NETP_ERR("[socket][%s]socket closed already, dial promise set abort, errno: %d", ch_info().c_str(), ch_errno() );
			status = netp::E_ECONNABORTED;
			goto _set_fail_and_return;
		}

		status = load_sockname();
		if (status != netp::OK ) {
			goto _set_fail_and_return;
		}

#ifdef _NETP_WIN
		//not sure linux os behaviour, to test
		if (0 == local_addr().ipv4() && m_type != u8_t(NETP_SOCK_USERPACKET/*FOR BFR*/) ) {
			status = netp::E_WSAENOTCONN;
			goto _set_fail_and_return;
		}
#endif
		netp::address raddr;
		status = netp::getpeername(*m_api, m_fd, raddr);
		if (status != netp::OK ) {
			goto _set_fail_and_return;
		}
		if (raddr != m_raddr) {
			status = netp::E_UNKNOWN;
			goto _set_fail_and_return;
		}

		if (local_addr() == remote_addr()) {
			status = netp::E_SOCKET_SELF_CONNCTED;
			NETP_WARN("[socket][%s]socket selfconnected", ch_info().c_str());
			goto _set_fail_and_return;
		}

		try {
			if (NETP_LIKELY(fn_initializer != nullptr)) {
				fn_initializer(NRP<channel>(this));
			}
		} catch(netp::exception const& e) {
			NETP_ASSERT(e.code() != netp::OK );
			status = e.code();
			goto _set_fail_and_return;
		} catch(std::exception const& e) {
			status = netp_socket_get_last_errno();
			if (status == netp::OK) {
				status = netp::E_UNKNOWN;
			}
			NETP_ERR("[socket][%s]dial error: %d: %s", ch_info().c_str(), status, e.what());
			goto _set_fail_and_return;
		} catch(...) {
			status = netp_socket_get_last_errno();
			if (status == netp::OK) {
				status = netp::E_UNKNOWN;
			}
			NETP_ERR("[socket][%s]dial error: %d: unknown", ch_info().c_str(), status);
			goto _set_fail_and_return;
		}

		NETP_TRACE_SOCKET("[socket][%s]async connected", ch_info().c_str());
		NETP_ASSERT( m_chflag& (int(channel_flag::F_CONNECTING)|int(channel_flag::F_CONNECTED)) );
		
		ch_set_connected();
		ch_io_end_connect();

		dialp->set(netp::OK);

		ch_fire_connected();
		ch_io_read();
	}

	void socket_channel::__cb_io_accept_impl(NRP<socket_cfg> const& cfg, fn_channel_initializer_t const& fn_initializer, int status, io_ctx* ) {

		NETP_ASSERT(L->in_event_loop());
		/*ignore the left fds, cuz we're closed*/
		if (NETP_UNLIKELY( m_chflag&int(channel_flag::F_CLOSED)) ) { return; }

		NETP_ASSERT(fn_initializer != nullptr);
		while (status == netp::OK) {
			address raddr;
			SOCKET nfd = socket_base::accept(raddr);
			if (nfd == NETP_SOCKET_ERROR) {
				status = netp_socket_get_last_errno();
				if (status == netp::E_EINTR) {
					status = netp::OK;
					continue;
				} else {
					break;
				}
			}

			//patch for local addr
			address laddr;
			status = netp::getsockname(*m_api, nfd, laddr);
			if (status != netp::OK) {
				NETP_ERR("[socket][%s][accept]load local addr failed: %d", ch_info().c_str(), netp_socket_get_last_errno());
				NETP_CLOSE_SOCKET(nfd);
				continue;
			}

			NETP_ASSERT(laddr.family() == (m_family));
			if (laddr == raddr) {
				NETP_ERR("[socket][%s][accept]laddr == raddr, force close", ch_info().c_str());
				NETP_CLOSE_SOCKET(nfd);
				continue;
			}

			NRP<io_event_loop> LL = io_event_loop_group::instance()->next(L->poller_type());
			LL->execute([LL,fn_initializer,nfd, laddr, raddr, cfg]() {
				std::tuple<int, NRP<socket_channel>> tupc = accepted_create<socket_channel>(LL,nfd, laddr,raddr, cfg);
				int rt = std::get<0>(tupc);
				if (rt != netp::OK) {
					NETP_CLOSE_SOCKET(nfd);
				}
				NRP<socket_channel> const& ch = std::get<1>(tupc);
				ch->__do_accept_fire(fn_initializer);
			});
		}

		if (IS_ERRNO_EQUAL_WOULDBLOCK(status)) {
			//TODO: check the following errno
			//ENETDOWN, EPROTO,ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH, EOPNOTSUPP
			return;
		}

		if(status == netp::E_EMFILE ) {
			NETP_WARN("[socket][%s]accept error, EMFILE", ch_info().c_str() );
			return ;
		}

		NETP_ERR("[socket][%s]accept error: %d", ch_info().c_str(), status);
		ch_errno()=(status);
		m_chflag |= int(channel_flag::F_READ_ERROR);
		ch_close_impl(nullptr);
	}

	void socket_channel::__cb_io_read_from_impl(int status, io_ctx* ) {
		NETP_ASSERT(m_protocol == u8_t(NETP_PROTOCOL_UDP));
		while (status == netp::OK) {
			NETP_ASSERT((m_chflag & (int(channel_flag::F_READ_SHUTDOWNING))) == 0);
			if (NETP_UNLIKELY(m_chflag & (int(channel_flag::F_READ_SHUTDOWN) | int(channel_flag::F_CLOSE_PENDING)/*ignore the left read buffer, cuz we're closing it*/))) { return; }
			netp::u32_t nbytes = socket_base::recvfrom(m_rcv_buf_ptr, m_rcv_buf_size, m_raddr, status);
			if (NETP_LIKELY(nbytes > 0)) {
				channel::ch_fire_readfrom(netp::make_ref<netp::packet>(m_rcv_buf_ptr, nbytes), m_raddr) ;
			}
		}
		___io_read_impl_done(status);
	}

	void socket_channel::__cb_io_read_impl(int status, io_ctx*) {
		//NETP_INFO("READ IN");
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(!ch_is_listener());

		//in case socket object be destructed during ch_read
		while (status == netp::OK) {
			NETP_ASSERT( (m_chflag&(int(channel_flag::F_READ_SHUTDOWNING))) == 0);
			if (NETP_UNLIKELY(m_chflag & (int(channel_flag::F_READ_SHUTDOWN)|int(channel_flag::F_READ_ERROR) | int(channel_flag::F_CLOSE_PENDING) | int(channel_flag::F_CLOSING)/*ignore the left read buffer, cuz we're closing it*/))) { return; }
			netp::u32_t nbytes = socket_base::recv(m_rcv_buf_ptr, m_rcv_buf_size, status);
			if (NETP_LIKELY(nbytes > 0)) {
				channel::ch_fire_read(netp::make_ref<netp::packet>(m_rcv_buf_ptr, nbytes));
			}
		}
		___io_read_impl_done(status);
	}

	void socket_channel::__cb_io_write_impl( int status, io_ctx*) {
		NETP_ASSERT( (m_chflag&(int(channel_flag::F_WRITE_SHUTDOWNING)|int(channel_flag::F_BDLIMIT)|int(channel_flag::F_CLOSING) )) == 0 );
		//NETP_TRACE_SOCKET("[socket][%s]__cb_io_write_impl, write begin: %d, flag: %u", ch_info().c_str(), status , m_chflag );
		if (status == netp::OK) {
			if (m_chflag&int(channel_flag::F_WRITE_ERROR)) {
				NETP_ASSERT(m_outbound_entry_q.size() != 0);
				NETP_ASSERT(ch_errno() != netp::OK);
				NETP_WARN("[socket][%s]__cb_io_write_impl(), but socket error already: %d, m_chflag: %u", ch_info().c_str(), ch_errno(), m_chflag);
				return ;
			} else if (m_chflag&(int(channel_flag::F_WRITE_SHUTDOWN))) {
				NETP_ASSERT( m_outbound_entry_q.size() == 0);
				NETP_WARN("[socket][%s]__cb_io_write_impl(), but socket write closed already: %d, m_chflag: %u", ch_info().c_str(), ch_errno(), m_chflag);
				return;
			} else {
				m_chflag |= int(channel_flag::F_WRITING);
				status = (is_udp() ? socket_channel::_do_ch_write_to_impl() : socket_channel::_do_ch_write_impl());
				m_chflag &= ~int(channel_flag::F_WRITING);
			}
		}
		__handle_io_write_impl_done(status);
	}

	//write until error
	//<0, is_error == (errno != E_CHANNEL_WRITING)
	//==0, write done
	//this api would be called right after a check of writeable of the current socket
	int socket_channel::_do_ch_write_impl() {

		NETP_ASSERT(m_outbound_entry_q.size() != 0, "%s, flag: %u", ch_info().c_str(), m_chflag);
		NETP_ASSERT( m_chflag&(int(channel_flag::F_WRITE_BARRIER)|int(channel_flag::F_WATCH_WRITE)) );
		NETP_ASSERT( (m_chflag&int(channel_flag::F_BDLIMIT)) ==0);

		//there might be a chance to be blocked a while in this loop, if set trigger another write
		int _errno = netp::OK;
		while ( _errno == netp::OK && m_outbound_entry_q.size() ) {
			NETP_ASSERT( (m_noutbound_bytes) > 0);
			socket_outbound_entry& entry = m_outbound_entry_q.front();
			u32_t dlen = u32_t(entry.data->len());
			u32_t wlen = (dlen);
			if (m_outbound_limit !=0 && (m_outbound_budget<wlen)) {
				wlen =m_outbound_budget;
				if (wlen == 0) {
					NETP_ASSERT(m_chflag& int(channel_flag::F_BDLIMIT_TIMER));
					return netp::E_CHANNEL_BDLIMIT;
				}
			}

			NETP_ASSERT((wlen > 0) && (wlen <= m_noutbound_bytes));
			netp::u32_t nbytes = socket_base::send(entry.data->head(), u32_t(wlen), _errno);
			if (NETP_LIKELY(nbytes > 0)) {
				m_noutbound_bytes -= nbytes;
				if (m_outbound_limit != 0 ) {
					m_outbound_budget -= nbytes;

					if (!(m_chflag & int(channel_flag::F_BDLIMIT_TIMER)) && m_outbound_budget < (m_outbound_limit >> 1)) {
						m_chflag |= int(channel_flag::F_BDLIMIT_TIMER);
						L->launch(netp::make_ref<netp::timer>(std::chrono::milliseconds(NETP_SOCKET_BDLIMIT_TIMER_DELAY_DUR), &socket_channel::_tmcb_BDL, NRP<socket_channel>(this), std::placeholders::_1));
					}
				}

				if (NETP_LIKELY(nbytes == dlen)) {
					NETP_ASSERT(_errno == netp::OK);
					entry.write_promise->set(netp::OK);
					m_outbound_entry_q.pop_front();
				} else {
					entry.data->skip(nbytes); //ewouldblock or bdlimit
					NETP_ASSERT(entry.data->len());
				}
			}
		}
		return _errno;
	}

	int socket_channel::_do_ch_write_to_impl() {

		NETP_ASSERT(m_outbound_entry_q.size() != 0, "%s, flag: %u", ch_info().c_str(), m_chflag);
		NETP_ASSERT(m_chflag & (int(channel_flag::F_WRITE_BARRIER)|int(channel_flag::F_WATCH_WRITE)));

		//there might be a chance to be blocked a while in this loop, if set trigger another write
		int _errno = netp::OK;
		while (_errno == netp::OK && m_outbound_entry_q.size() ) {
			NETP_ASSERT(m_noutbound_bytes > 0);

			socket_outbound_entry& entry = m_outbound_entry_q.front();
			NETP_ASSERT((entry.data->len() > 0) && (entry.data->len() <= m_noutbound_bytes));
			netp::u32_t nbytes = socket_base::sendto(entry.data->head(), (u32_t)entry.data->len(), entry.to, _errno);
			//hold a copy before we do pop it from queue
			nbytes == entry.data->len() ? NETP_ASSERT(_errno == netp::OK):NETP_ASSERT(_errno != netp::OK);
			m_noutbound_bytes -= u32_t(entry.data->len());
			entry.write_promise->set(_errno);
			m_outbound_entry_q.pop_front();
		}
		return _errno;
	}

	void socket_channel::_ch_do_close_listener() {
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(m_chflag & int(channel_flag::F_LISTENING));
		NETP_ASSERT((m_chflag & int(channel_flag::F_CLOSED)) ==0 );

		m_chflag |= int(channel_flag::F_CLOSED);
		ch_io_end_accept();
		ch_io_end();
		NETP_TRACE_SOCKET("[socket][%s]ch_do_close_listener end", ch_info().c_str());
	}

	inline void socket_channel::_ch_do_close_read_write() {
		NETP_ASSERT(L->in_event_loop());

		if (m_chflag & (int(channel_flag::F_CLOSING) | int(channel_flag::F_CLOSED))) {
			return;
		}

		//NETP_ASSERT((m_chflag & int(channel_flag::F_CLOSED)) == 0);
		m_chflag |= int(channel_flag::F_CLOSING);
		m_chflag &= ~(int(channel_flag::F_CLOSE_PENDING) | int(channel_flag::F_CONNECTED));
		NETP_TRACE_SOCKET("[socket][%s]ch_do_close_read_write, errno: %d, flag: %d", ch_info().c_str(), ch_errno(), m_chflag);

		_ch_do_close_read();
		_ch_do_close_write();

		m_chflag &= ~int(channel_flag::F_CLOSING);

		NETP_ASSERT(m_outbound_entry_q.size() == 0);
		NETP_ASSERT(m_noutbound_bytes == 0);

		//close read, close write might result in F_CLOSED
		ch_rdwr_shutdown_check();
	}

	void socket_channel::ch_close_write_impl(NRP<promise<int>> const& closep) {
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(!ch_is_listener());
		int prt = netp::OK;
		if (m_chflag & (int(channel_flag::F_WRITE_SHUTDOWNING)|int(channel_flag::F_CLOSING)| int(channel_flag::F_CLOSE_PENDING)) ) {
			prt = (netp::E_OP_INPROCESS);
		} else if (m_chflag&int(channel_flag::F_WRITE_SHUTDOWN_PENDING)) {
			NETP_ASSERT((m_chflag&int(channel_flag::F_WRITE_ERROR)) == 0);
			NETP_ASSERT(m_chflag&(int(channel_flag::F_WRITING)|int(channel_flag::F_WATCH_WRITE)) );
			NETP_ASSERT(m_outbound_entry_q.size());
			prt = (netp::E_CHANNEL_WRITE_SHUTDOWNING);
		} else if (m_chflag & int(channel_flag::F_WRITE_SHUTDOWN)) {
			prt = (netp::E_CHANNEL_WRITE_CLOSED);
		} else if (m_chflag & (int(channel_flag::F_WRITING)|int(channel_flag::F_WATCH_WRITE)|int(channel_flag::F_BDLIMIT)) ) {
			//write set ok might trigger ch_close_write|ch_close
			NETP_ASSERT(m_outbound_entry_q.size());
			m_chflag |= int(channel_flag::F_WRITE_SHUTDOWN_PENDING);
			prt = (netp::E_CHANNEL_WRITE_SHUTDOWNING);
		} else {
			NETP_ASSERT(((m_chflag&(int(channel_flag::F_WRITE_ERROR) | int(channel_flag::F_CONNECTED))) == (int(channel_flag::F_WRITE_ERROR) | int(channel_flag::F_CONNECTED))) ?
				m_outbound_entry_q.size():
				true);

			_ch_do_close_write();
		}

		if (closep) { closep->set(prt); }
	}

	void socket_channel::ch_close_impl(NRP<promise<int>> const& closep) {
		NETP_ASSERT(L->in_event_loop());
		int prt = netp::OK;
		if (m_chflag&int(channel_flag::F_CLOSED)) {
			prt = (netp::E_CHANNEL_CLOSED);
		} else if (ch_is_listener()) {
			_ch_do_close_listener();
		} else if (m_chflag & (int(channel_flag::F_CLOSE_PENDING)|int(channel_flag::F_CLOSING)) ) {
			prt = (netp::E_OP_INPROCESS);
		} else if (m_chflag & (int(channel_flag::F_WRITING)|int(channel_flag::F_WATCH_WRITE)|int(channel_flag::F_BDLIMIT)) ) {
			//wait for write done event
			NETP_ASSERT( ((m_chflag&int(channel_flag::F_WRITE_ERROR)) == 0) );

			NETP_ASSERT(m_outbound_entry_q.size());
			m_chflag |= int(channel_flag::F_CLOSE_PENDING);
			prt = (netp::E_CHANNEL_CLOSING);
		} else {
			if ((m_chflag & int(channel_flag::F_CONNECTING)) && ch_errno() == netp::OK) {
				m_chflag |= int(channel_flag::F_WRITE_ERROR);
				ch_errno() = (netp::E_CHANNEL_ABORT);
			}

			if (ch_errno() != netp::OK) {
				NETP_ASSERT(m_chflag & (int(channel_flag::F_READ_ERROR) | int(channel_flag::F_WRITE_ERROR) | int(channel_flag::F_IO_EVENT_LOOP_NOTIFY_TERMINATING)));
				NETP_ASSERT( ((m_chflag&(int(channel_flag::F_WRITE_ERROR) | int(channel_flag::F_CONNECTED))) == (int(channel_flag::F_WRITE_ERROR) | int(channel_flag::F_CONNECTED))) ?
					m_outbound_entry_q.size() :
					true);
				//only check write error for q.size
			} else {
				NETP_ASSERT(m_outbound_entry_q.size() == 0);
			}
			_ch_do_close_read_write();
		}

		if (closep) { closep->set(prt); }
	}

#define __CH_WRITEABLE_CHECK__( outlet, chp)  \
		NETP_ASSERT(outlet->len() > 0); \
		NETP_ASSERT(chp != nullptr); \
 \
		if (m_chflag&(int(channel_flag::F_READ_ERROR) | int(channel_flag::F_WRITE_ERROR))) { \
			chp->set(netp::E_CHANNEL_READ_WRITE_ERROR); \
			return ; \
		} \
 \
		if ((m_chflag&int(channel_flag::F_WRITE_SHUTDOWN)) != 0) { \
			chp->set(netp::E_CHANNEL_WRITE_CLOSED); \
			return; \
		} \
 \
		if (m_chflag&(int(channel_flag::F_WRITE_SHUTDOWN_PENDING)|int(channel_flag::F_WRITE_SHUTDOWNING) | int(channel_flag::F_CLOSE_PENDING) | int(channel_flag::F_CLOSING)) ) { \
			chp->set(netp::E_CHANNEL_WRITE_SHUTDOWNING); \
			return ; \
		} \
 \
		const u32_t outlet_len = (u32_t)outlet->len(); \
		/*set the threshold arbitrarily high, the writer have to check the return value if */ \
		if ( (m_noutbound_bytes > 0) && ( (m_noutbound_bytes + outlet_len) > /*m_sock_buf.sndbuf_size,*/u32_t(channel_buf_range::CH_BUF_SND_MAX_SIZE))) { \
			NETP_ASSERT(m_noutbound_bytes > 0); \
			NETP_ASSERT(m_chflag&(int(channel_flag::F_WRITE_BARRIER)|int(channel_flag::F_WATCH_WRITE))); \
			chp->set(netp::E_CHANNEL_WRITE_BLOCK); \
			return; \
		} \

	void socket_channel::ch_write_impl(NRP<packet> const& outlet, NRP<promise<int>> const& chp)
	{
		NETP_ASSERT(L->in_event_loop());
		__CH_WRITEABLE_CHECK__(outlet, chp)
		m_outbound_entry_q.push_back({
			netp::make_ref<netp::packet>(outlet->head(), outlet_len),
			chp
		});
		m_noutbound_bytes += outlet_len;

		if (m_chflag&(int(channel_flag::F_WRITE_BARRIER)|int(channel_flag::F_WATCH_WRITE)|int(channel_flag::F_BDLIMIT))) {
			return;
		}

#ifdef NETP_ENABLE_FAST_WRITE
		//fast write
		m_chflag |= int(channel_flag::F_WRITE_BARRIER);
		__cb_io_write_impl(netp::OK, m_io_ctx);
		m_chflag &= ~int(channel_flag::F_WRITE_BARRIER);
#else
		ch_io_write();
#endif
	}

	void socket_channel::ch_write_to_impl(NRP<packet> const& outlet, netp::address const& to, NRP<promise<int>> const& chp) {
		NETP_ASSERT(L->in_event_loop());

		__CH_WRITEABLE_CHECK__(outlet, chp)
		m_outbound_entry_q.push_back({
			netp::make_ref<netp::packet>(outlet->head(), outlet_len),
			chp,
			to,
		});
		m_noutbound_bytes += outlet_len;

		if (m_chflag & (int(channel_flag::F_WRITE_BARRIER)|int(channel_flag::F_WATCH_WRITE)) ) {
			return;
		}

#ifdef NETP_ENABLE_FAST_WRITE
		//fast write
		m_chflag |= int(channel_flag::F_WRITE_BARRIER);
		__cb_io_write_impl(netp::OK,m_io_ctx);
		m_chflag &= ~int(channel_flag::F_WRITE_BARRIER);
#else
		ch_io_write();
#endif
	}

	void socket_channel::io_notify_terminating(int status, io_ctx*) {
		NETP_ASSERT(L->in_event_loop());
		NETP_ASSERT(status == netp::E_IO_EVENT_LOOP_NOTIFY_TERMINATING);
		//terminating notify, treat as a error
		NETP_ASSERT(m_chflag & int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE));
		m_chflag |= (int(channel_flag::F_IO_EVENT_LOOP_NOTIFY_TERMINATING));
		m_cherrno = netp::E_IO_EVENT_LOOP_NOTIFY_TERMINATING;
		m_chflag &= ~(int(channel_flag::F_CLOSE_PENDING) | int(channel_flag::F_BDLIMIT));
		ch_close_impl(nullptr);
	}

	void socket_channel::io_notify_read(int status, io_ctx* ctx) {
		if (m_chflag & int(channel_flag::F_USE_DEFAULT_READ)) {
			is_udp ? __cb_io_read_from_impl(status, ctx):__cb_io_read_impl(status, ctx);
			return;
		}
		NETP_ASSERT( m_fn_read != nullptr );
		(*m_fn_read)(status, ctx);
	}

	void socket_channel::io_notify_write(int status, io_ctx* ctx) {
		if (m_chflag & int(channel_flag::F_USE_DEFAULT_WRITE)) {
			__cb_io_write_impl(status, ctx);
			return;
		}
		NETP_ASSERT(m_fn_write != nullptr);
		(*m_fn_write)(status, ctx);
	}

	void socket_channel::__ch_clean() {
		NETP_ASSERT(m_fn_read == nullptr);
		NETP_ASSERT(m_fn_write == nullptr);
		ch_deinit();
		if (m_chflag & int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE)) {
			L->io_end(m_io_ctx);
		}
	}

	void socket_channel::ch_io_begin(fn_io_event_t const& fn_begin_done) {
		NETP_ASSERT(is_nonblocking());

		if (!L->in_event_loop()) {
			L->schedule([s = NRP<socket_channel>(this), fn_begin_done]() {
				s->ch_io_begin(fn_begin_done);
			});
			return;
		}

		NETP_ASSERT((m_chflag & (int(channel_flag::F_IO_EVENT_LOOP_BEGIN_DONE))) == 0);

		m_io_ctx = L->io_begin(m_fd, NRP<io_monitor>(this));
		if (m_io_ctx == 0) {
			ch_errno() = netp::E_IO_BEGIN_FAILED;
			m_chflag |= int(channel_flag::F_READ_ERROR);//for assert check
			ch_close(nullptr);
			fn_begin_done(netp::E_IO_BEGIN_FAILED, 0);
			return;
		}
		__io_begin_done(m_io_ctx);
		fn_begin_done(netp::OK, m_io_ctx);
	}

		void socket_channel::ch_io_end() {
			NETP_ASSERT(L->in_event_loop());
			NETP_ASSERT(m_outbound_entry_q.size() == 0);
			NETP_ASSERT(m_noutbound_bytes == 0);
			NETP_ASSERT(m_chflag & int(channel_flag::F_CLOSED));
			NETP_ASSERT((m_chflag & (int(channel_flag::F_WATCH_READ) | int(channel_flag::F_WATCH_WRITE))) == 0);
			NETP_TRACE_SOCKET("[socket][%s]io_action::END, flag: %d", ch_info().c_str(), m_chflag);

			ch_fire_closed(close());
			//delay one tick to hold this
			L->schedule([so = NRP<socket_channel>(this)]() {
				so->__ch_clean();
			});
		}

		void socket_channel::ch_io_accept(fn_io_event_t const& fn ) {
			if (!L->in_event_loop()) {
				L->schedule([ch = NRP<channel>(this), fn]() {
					ch->ch_io_accept(fn);
				});
				return;
			}
			NETP_ASSERT(L->in_event_loop());

			if (m_chflag & int(channel_flag::F_WATCH_READ)) {
				NETP_TRACE_SOCKET("[socket][%s][_do_io_accept]F_WATCH_READ state already", ch_info().c_str());
				return;
			}

			if (m_chflag & int(channel_flag::F_READ_SHUTDOWN)) {
				NETP_TRACE_SOCKET("[socket][%s][_do_io_accept]cancel for rd closed already", ch_info().c_str());
				return;
			}
			NETP_TRACE_SOCKET("[socket][%s][_do_io_accept]watch IO_READ", ch_info().c_str());

			//@TODO: provide custome accept feature
			//const fn_io_event_t _fn = cb_accepted == nullptr ? std::bind(&socket::__cb_async_accept_impl, NRP<socket>(this), std::placeholders::_1) : cb_accepted;
			int rt = L->io_do(io_action::READ, m_io_ctx);
			if (NETP_UNLIKELY(rt != netp::OK)) {
				return;
			}

			m_chflag &= ~int(channel_flag::F_USE_DEFAULT_WRITE);
			m_chflag |= int(channel_flag::F_WATCH_READ);
			m_fn_read = netp::allocator<fn_io_event_t>::make(fn);
		}

		void socket_channel::ch_io_read(fn_io_event_t const& fn_read) {
			if (!L->in_event_loop()) {
				L->schedule([s = NRP<socket_channel>(this), fn_read]()->void {
					s->ch_io_read(fn_read);
				});
				return;
			}
			NETP_ASSERT((m_chflag & int(channel_flag::F_READ_SHUTDOWNING)) == 0);
			if (m_chflag & int(channel_flag::F_WATCH_READ)) {
				NETP_TRACE_SOCKET("[socket][%s]io_action::READ, ignore, flag: %d", ch_info().c_str(), m_chflag);
				return;
			}

			if (m_chflag & int(channel_flag::F_READ_SHUTDOWN)) {
				NETP_ASSERT((m_chflag & int(channel_flag::F_WATCH_READ)) == 0);
				if (fn_read != nullptr) fn_read(netp::E_CHANNEL_READ_CLOSED, nullptr);
				return;
			}
			int rt = L->io_do(io_action::READ, m_io_ctx);
			if (NETP_UNLIKELY(rt != netp::OK)) {
				if(fn_read != nullptr) fn_read(rt, nullptr);
				return;
			}
			if (fn_read == nullptr) {
				m_chflag |= (int(channel_flag::F_USE_DEFAULT_READ)|int(channel_flag::F_WATCH_READ));
			} else {
				m_chflag &= ~int(channel_flag::F_USE_DEFAULT_READ);
				m_chflag |= int(channel_flag::F_WATCH_READ);
				m_fn_read = netp::allocator<fn_io_event_t>::make(fn_read);
			}
			NETP_TRACE_IOE("[socket][%s]io_action::READ", ch_info().c_str());
		}

		void socket_channel::ch_io_end_read() {
			if (!L->in_event_loop()) {
				L->schedule([_so = NRP<socket_channel>(this)]()->void {
					_so->ch_io_end_read();
				});
				return;
			}

			if ((m_chflag & int(channel_flag::F_WATCH_READ))) {
				L->io_do(io_action::END_READ, m_io_ctx);
				m_chflag &= ~(int(channel_flag::F_USE_DEFAULT_READ) | int(channel_flag::F_WATCH_READ));
				netp::allocator<fn_io_event_t>::trash(m_fn_read);
				m_fn_read = nullptr;
				NETP_TRACE_IOE("[socket][%s]io_action::END_READ", ch_info().c_str());
			}
		}

		void socket_channel::ch_io_write(fn_io_event_t const& fn_write ) {
			if (!L->in_event_loop()) {
				L->schedule([_so = NRP<socket_channel>(this), fn_write]()->void {
					_so->ch_io_write(fn_write);
				});
				return;
			}

			if (m_chflag & int(channel_flag::F_WATCH_WRITE)) {
				NETP_ASSERT(m_chflag & int(channel_flag::F_CONNECTED));
				if (fn_write != nullptr) {
					fn_write(netp::E_SOCKET_OP_ALREADY, 0);
				}
				return;
			}

			if (m_chflag & int(channel_flag::F_WRITE_SHUTDOWN)) {
				NETP_ASSERT((m_chflag & int(channel_flag::F_WATCH_WRITE)) == 0);
				NETP_TRACE_SOCKET("[socket][%s]io_action::WRITE, cancel for wr closed already", ch_info().c_str());
				if (fn_write != nullptr) {
					fn_write(netp::E_CHANNEL_WRITE_CLOSED, 0);
				}
				return;
			}

			int rt = L->io_do(io_action::WRITE, m_io_ctx);
			if (NETP_UNLIKELY(rt != netp::OK)) {
				if (fn_write != nullptr) fn_write(rt, nullptr);
				return;
			}
			if (fn_write == nullptr) {
				m_chflag |= (int(channel_flag::F_USE_DEFAULT_WRITE) | int(channel_flag::F_WATCH_WRITE));
			} else {
				m_chflag &= ~int(channel_flag::F_USE_DEFAULT_WRITE);
				m_chflag |= int(channel_flag::F_WATCH_WRITE);
				m_fn_write = netp::allocator<fn_io_event_t>::make(fn_write);
			}
			NETP_TRACE_IOE("[socket][%s]io_action::WRITE", ch_info().c_str());
		}

		void socket_channel::ch_io_end_write() {
			if (!L->in_event_loop()) {
				L->schedule([_so = NRP<socket_channel>(this)]()->void {
					_so->ch_io_end_write();
				});
				return;
			}

			if (m_chflag & int(channel_flag::F_WATCH_WRITE)) {
				L->io_do(io_action::END_WRITE, m_io_ctx);
				m_chflag &= ~(int(channel_flag::F_USE_DEFAULT_WRITE) | int(channel_flag::F_WATCH_WRITE));
				netp::allocator<fn_io_event_t>::trash(m_fn_write);
				m_fn_write = nullptr;
				NETP_TRACE_IOE("[socket][%s]io_action::END_WRITE", ch_info().c_str());
			}
		}
} //end of ns
