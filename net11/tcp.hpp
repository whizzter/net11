#include <functional>
#include <exception>
#include <memory>
#include <vector>
#include <array>
#include <utility>

#include <algorithm>

#include <stdio.h>

#ifdef _MSC_VER
#include <windows.h>
#include <winsock.h>
#pragma comment(lib,"wsock32.lib")
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>
#define closesocket(x) close(x)
#endif

namespace net11 {

	static void set_non_blocking_socket(int socket) {
	#ifdef _MSC_VER
		unsigned long nbl=1;
		ioctlsocket(socket,FIONBIO,&nbl);
	#else
		int flags=fcntl(socket,F_GETFL);
		flags|=O_NONBLOCK;
		fcntl(socket,F_SETFL,flags);
	#endif
	}

	template<typename T>
	std::function<bool(std::vector<char> &)> make_data_producer(T &in_data) {
		std::shared_ptr<int> off(new int);
		std::shared_ptr<T> data(new T(in_data));
		*off=0;
		return [data,off](std::vector<char> &ob){
			int dl=data->size()-*off;
			int ol=ob.capacity()-ob.size();
			int to_copy=dl<ol?dl:ol;
			std::copy_n(std::next(data->begin(),*off),to_copy,std::back_inserter(ob));
			*off+=to_copy;
			return *off!=data->size();
		};
	}

	std::function<bool(std::vector<char>&)> make_line_sink(const char * term,int max,std::function<bool(std::string&)> f) {
		std::shared_ptr<std::string> p(new std::string());
		size_t tl=std::strlen(term);
		return [p,tl,term,max,f](std::vector<char> &data) {
			size_t sz=p->size();
			for (int i=0;i<data.size();i++) {
				if (sz>=max) {
					return false;
				}
				p->push_back(data[i]);
				sz++;
				if (sz>tl) {
					if (!memcmp(p->data()+sz-tl,term,tl)) {
						p->resize(sz-tl);
						bool rv=f(*p);
						data.erase(data.begin(),data.begin()+sz);
						p->resize(0);
						return rv;
					}
				}
			}
			data.clear();
			return true;
		};
	}

	std::function<bool(std::vector<char>&)> make_header_sink(int maxk,int maxv,std::function<bool(std::string&,std::string&)> f,std::function<bool(const char *err)> fin) {
		enum headerstate {
			firstlinestart=0,
			linestart,
			testemptyline,
			inkey,
			postkeyskip,
			invalue,
			postvalue
		};
		struct head {
			headerstate state;
			std::string k;
			std::string v;
		};
		std::shared_ptr<head> p(new head);
		p->state=firstlinestart;
		return [p,maxk,maxv,f,fin](std::vector<char> &data) {
			for (int i=0;i<data.size();i++) {
				char c=data[i];
				switch(p->state) {
				case firstlinestart :
				case linestart :
					if (c==13) {
						// empty line in progress
						p->state=testemptyline;
						continue;
					} else if (c==10) {
						return fin("spurios LF");
					}
					if (p->state!=firstlinestart) {
						if (isspace(c)) {
							p->state=invalue;
							p->v.push_back(c);
							continue;
						}
						f(p->k,p->v);
						p->k.clear();
						p->v.clear();
					}
					if (isspace(c))
						continue;
					p->state=inkey;
					p->k.push_back(c);
					continue;
				case testemptyline :
					if (c==10) {
						// empty line encountered, we're finished with the data
						bool rv=fin(nullptr);
						p->k.clear();
						p->v.clear();
						p->state=firstlinestart;
						return rv;
					} else {
						return fin("cr but no lf in empty headerline");
					}
				case inkey :
					if (c==':') {
						p->state=postkeyskip;
						continue;
					} else {
						p->k.push_back(c);
						continue;
					}
				case postkeyskip :
					if (isspace(c)) {
						continue;
					} else {
						p->state=invalue;
						p->v.push_back(c);
						continue;
					}
				case invalue :
					if (c==13) {
						p->state=postvalue;
						continue;
					} else {
						p->v.push_back(c);
						continue;
					}
				case postvalue :
					if (c==10) {
						p->state=linestart;
						continue;
					} else {
						return fin("cr but no lf in headerline");
					}
				default:
					printf("headerparser unhandled state:%d\n",p->state);
					exit(-1);
				}
			}
			return true;
		};
	}

	class connection {
	public:
		connection() {
			//printf("conn ctor\n");
		}
		virtual ~connection() {
			//printf("conn dtor\n");
		}
		std::function<bool(std::vector<char>&)> sink;
		std::vector<std::function<bool(std::vector<char>&)> > producers;
	};

	class tcp {
		std::vector<std::pair<int,std::function<connection*()>>> listeners;
		class tcpconn {
			friend tcp;
			int sock;
			// unique_ptr is broken, ifdef?
			std::shared_ptr<connection> conn;
			bool want_input;
			std::vector<char> input;
			std::vector<char> output;
		};
		std::vector<tcpconn> conns;

		bool was_block() {
#ifdef _MSC_VER
			return (WSAGetLastError()==WSAEWOULDBLOCK) ;
#else
			return (errno==EAGAIN) ;
#endif
		}

		int input_buffer_size;
		int output_buffer_size;
		bool work_conn(tcpconn &c) {
			if (c.want_input) {
				int old_size=c.input.size();
				int cap=c.input.capacity();
				c.input.resize(cap);
				int rc=recv(c.sock,c.input.data()+old_size,cap-old_size,0);
				if (rc<0) {
					if (was_block()) {
						//printf("WOULD BLOCK\n");
					} else {
						// not a blocking error!
						return false;
					}
				} else if (rc>0) {
					c.input.resize(old_size+rc);
					while(c.input.size() && (c.want_input=c.conn->sink(c.input)));
				} else {
					// 0 on recv, closed sock;
					return false;
				}
			}
			while (c.output.size() || c.conn->producers.size()) {
				//printf("Has output:%d %d\n",c.output.size(),c.conn->producers.size());
				if (c.output.size()) {
					//printf("network output bytes:%d [",c.output.size());
					//for (int i=0;i<c.output.size();i++) {
					//	printf("%c",c.output[i]);
					//}
					//printf("]\n");
					int rc=send(c.sock,c.output.data(),c.output.size(),0);
					//printf("network send:%d\n",rc);
					if (rc<0) {
						if (!was_block()) {
							// error other than wouldblock
							return false;
						}
					} else if (rc>0) {
						bool brk=rc!=c.output.size();
						c.output.erase(c.output.begin(),c.output.begin()+rc);
						if (brk)
							break; // could not take all data, do more later
					}
				}
				if (c.output.size()<c.output.capacity() && c.conn->producers.size()) {
					if (!c.conn->producers.front()(c.output)) {
						c.conn->producers.erase(c.conn->producers.begin());
					}
				}
			}
			return c.want_input || c.output.size() || c.conn->producers.size();
		}
	public:
		tcp(int in_input_buffer_size=4096,int in_out_buffer_size=20):input_buffer_size(in_input_buffer_size),output_buffer_size(in_out_buffer_size) {
#ifdef _MSC_VER
			WSADATA wsa_data;
			if (WSAStartup(MAKEWORD(1,0),&wsa_data)) {
				throw new std::exception("WSAStartup problem");
			}
#endif
		}
		~tcp() {
#ifdef _MSC_VER
			if (WSACleanup()) {
				throw new std::exception("WSACleanup shutdown error\n");
			}
#endif
		}
		bool poll() {
			if(listeners.size()==0 && conns.size()==0)
				return false;
			// first see if we have any new connections
			for(auto l:listeners) {
				struct sockaddr_in addr;
#ifdef _MSC_VER
				int addrsize=sizeof(addr);
#else
				socklen_t addrsize=sizeof(addr);
#endif
				int newsock=accept(l.first,(struct sockaddr*)&addr,&addrsize);
				if (newsock!=-1) {
					set_non_blocking_socket(newsock);
					conns.emplace_back();
					conns.back().sock=newsock;
					conns.back().input.reserve(input_buffer_size);
					conns.back().output.reserve(input_buffer_size);
					conns.back().want_input=true;
					//conns.back().output?
					conns.back().conn.reset(l.second());
				}
			}
			// now see if we have new data
			// TODO: rewrite this to use kevent,etc
			conns.erase(
				std::remove_if(
					conns.begin(),
					conns.end(),
					[this](tcpconn &c) {
						//printf("running conn:%p\n",&c);
						if (!work_conn(c)) {
							//printf("Wanting to remove conn!\n");
							closesocket(c.sock);
							return true;
						} else {
							return false;
						}
					}
				),
				conns.end()
			);
			return true; // change this somehow?
		}
		bool listen(int port,std::function<connection*()> spawn) {
			int sock=-1;
			struct sockaddr_in sockaddr;
			if (-1==(sock=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP))) {
				return true;
			}
			memset(&sockaddr,0,sizeof(sockaddr));
			sockaddr.sin_family=AF_INET;
			sockaddr.sin_addr.s_addr=0; // default addr
			sockaddr.sin_port=htons(port);
			if (bind(sock,(struct sockaddr*)&sockaddr,sizeof(sockaddr))) {
				closesocket(sock);
				return true;
			}
			if (::listen(sock,SOMAXCONN)) {
				closesocket(sock);
				return true;
			}
			set_non_blocking_socket(sock);
			listeners.push_back(std::make_pair(sock,spawn));
			return false;
		}
	};

	void yield() {
		// TODO: IOCP/KEVENT...
	#ifdef _MSC_VER
		Sleep(1);
	#else
		usleep(10000);
	#endif
	}
}

