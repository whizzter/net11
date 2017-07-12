#include <functional>
#include <exception>
#include <vector>
#include <array>
#include <utility>

#include <algorithm>
#include <cstring>

//#include <stdio.h>

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

// include net11 utilities
#include "util.hpp"

namespace net11 {
	// the tcp class is a container for listeners and connections
	class tcp;
	// a single connection, protocols should inherit this class
	class connection;

	class tcp {
	public:
		class connection;
	private:
		std::vector<std::pair<int,std::function<connection*()>>> listeners;
		class tcpconn {
			friend tcp;
			int sock;
			std::shared_ptr<connection> conn;
			bool want_input;
			buffer input;
			buffer output;
			tcpconn(int insize,int outsize):input(insize),output(outsize) {}
		};
		std::vector<std::shared_ptr<tcpconn>> conns;

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

		static bool was_block() {
#ifdef _MSC_VER
			return (WSAGetLastError()==WSAEWOULDBLOCK) ;
#else
			return (errno==EAGAIN) ;
#endif
		}

		int input_buffer_size;
		int output_buffer_size;
		bool work_conn(tcpconn &c) {
			int fill_count = 0;
			while(c.want_input && fill_count<10) {
				// process events as long as we have data and don't have multiple
				// producers on queue to avoid denial of service scenarios where
				// the producers are too slow to drain before the sink has read.
				// Note: the sink should be called first since was_block below will break the loop on block.
				if (c.input.usage() && c.conn->producers.size()<=1) {
					c.want_input=c.conn->current_sink->drain(c.input);
					continue;
				}
				// try to fill up the buffer as much as possible.
				if (c.input.total_avail()) {
					int avail=c.input.compact();
					int rc=recv(c.sock,c.input.to_produce(),avail,0);
					if (rc<0) {
						if (was_block()) {
							//printf("WOULD BLOCK\n");
							break;
						} else {
							// not a blocking error!
							return false;
						}
					} else if (rc>0) {
						c.input.produced(rc);
						fill_count++;
						continue;
					} else {
						// 0 on recv, closed sock;
						return false;
					}
				}
				break;
			}
			while (c.output.usage() || c.conn->producers.size()) {
				if (c.output.usage()) {
#ifdef _MSC_VER
					const int send_flags=0;
#else
					const int send_flags=MSG_NOSIGNAL;
#endif
					int rc=send(c.sock,c.output.to_consume(),c.output.usage(),send_flags);
					if (rc<0) {
						if (!was_block()) {
							// error other than wouldblock
							return false;
						}
					} else if (rc>0) {
						//bool brk=rc!=c.output.usage();
						c.output.consumed(rc);
						//c.output.erase(c.output.begin(),c.output.begin()+rc);
						if (c.output.usage())
							break; // could not take all data, do more later
					}
				}
				if (c.output.total_avail() && c.conn->producers.size()) {
					int preuse=c.output.total_avail();
					if (!c.conn->producers.front()(c.output)) {
						// producer finished, remove it.
						c.conn->producers.erase(c.conn->producers.begin());
					} else if (preuse==c.output.total_avail()) {
						// no data was generated.
						break;
					}
				}
			}
			return c.want_input || c.output.usage() || c.conn->producers.size();
		}
	public:
		tcp(int in_input_buffer_size=4096,int in_out_buffer_size=4096):input_buffer_size(in_input_buffer_size),output_buffer_size(in_out_buffer_size) {
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
				while(true) {
					struct sockaddr_in addr;
#ifdef _MSC_VER
					int addrsize=sizeof(addr);
#else
					socklen_t addrsize=sizeof(addr);
#endif
					int newsock=accept(l.first,(struct sockaddr*)&addr,&addrsize);
					if (newsock!=-1) {
						set_non_blocking_socket(newsock);
						conns.push_back(std::shared_ptr<tcpconn>(new tcpconn(input_buffer_size,output_buffer_size)));
						conns.back()->sock=newsock;
						conns.back()->want_input=true;
						conns.back()->conn.reset(l.second());
						continue;
					} else {
						break;
					}
				}
			}
			// now see if we have new data
			// TODO: rewrite this to use kevent,etc
			conns.erase(
				std::remove_if(
					conns.begin(),
					conns.end(),
					[this](std::shared_ptr<tcpconn> c) {
						//printf("running conn:%p\n",&c);
						if (!work_conn(*c)) {
							//printf("Wanting to remove conn!\n");
							closesocket(c->sock);
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

		class connection :  public std::enable_shared_from_this<connection> {
		public:
			connection() {
				//printf("conn ctor\n");
			}
			virtual ~connection() {
				//printf("conn dtor\n");
			}
			//std::function<bool(std::vector<char>&)> *sink;
			std::shared_ptr<sink> current_sink;
			//std::vector<std::function<bool(std::vector<char>&)> > producers;
			std::vector<std::function<bool(buffer&)> > producers;
		};
	};
}

