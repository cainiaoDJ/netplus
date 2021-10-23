//������ͷ�ļ�
#include <netp.hpp>

//���ǵķ��������������ú����Ϸ���һ��NRP<netp::promise<int>>����promise����ȡ��һ��intֵ
//������˼򵥻����⣬������int�����ʵ���ϣ��������Ϳ���Ϊ��������
NRP<netp::promise<int>> do_watch_a_future_state() {
	NRP<netp::promise<int>> p = netp::make_ref<netp::promise<int>>();
	NRP<netp::timer> tm = netp::make_ref<netp::timer>(std::chrono::seconds(2), [p]() {
		p->set(8);
	});
	netp::app::instance()->def_loop_group()->launch(tm);
	return p;
}
int main(int argc, char** argv) {
	netp::app::instance()->init(argc, argv);
	netp::app::instance()->start_loop();
	//���������ÿ�ʼ
	NRP<netp::promise<int>> intp = do_watch_a_future_state();

	//��һ�п�ʼ������Ϊ��promise����һ��lambda, ����ʱ��������ʱ�򣬴�lambda��ִ�С�
	//ע�⣺if_doneҲ�Ƿ������ģ�������ɺ������Ϸ��أ�ʹ�����ǵ��߳̿���ȥִ������������ 
	//��Ȼ��ͨ����intp->get()Ҳ����std::future�Ǹ�ȡ��һ��ֵ�����ǣ���������ˡ�
	intp->if_done([](int i) {
		NETP_INFO("do_async return : %d", i);
	});

	//�ȴ��˳��ź�
	//ctrl+c, kill -15
	netp::app::instance()->start_loop();
	return 0;
}