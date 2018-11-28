#pragma once

#include <functional>
#include <exception>
#include <vector>
#include <array>
#include <utility>

#include <algorithm>
#include <cstring>

//#include <stdio.h>

//#define NET11_VERBOSE

#ifdef _MSC_VER

//#define NET11_OVERLAPPED
#ifdef NET11_VERBOSE
#define NET11_TCP_LOG(...) fprintf(stderr,__VA_ARGS__);
#else
#define NET11_TCP_LOG(...)
#endif

// remove problematic windows min / max macros
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
//#include <winsock.h>
//#pragma comment(lib,"wsock32.lib")
#pragma comment(lib,"ws2_32.lib")
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
		std::vector<std::pair<int, std::function<void(connection*)>>> listeners;
	public:
		class connection {
			friend tcp;
			int sock;
			//std::shared_ptr<connection> conn;
			bool want_input;
			buffer input;
			buffer output;
#ifdef NET11_OVERLAPPED
			bool ol_input_pending = false;
			bool ol_output_pending = false;
			WSABUF wsa_input;
			WSAOVERLAPPED overlapped_input;
			WSABUF wsa_output;
			WSAOVERLAPPED overlapped_output;
#endif
			connection(int insize, int outsize) :input(insize), output(outsize) {}
		//public:
			~connection() {
				NET11_TCP_LOG("Socket %x killed\n", sock);
			}
			struct deleter {
				void operator()(connection* p) {
					delete p;
				}
			};
		public:
			std::shared_ptr<sink> current_sink;
			std::function<void()> terminate;
			std::vector<std::function<bool(buffer&)> > producers;
			std::shared_ptr<void> ctx;
		};
	private:
		std::vector<std::unique_ptr<connection,connection::deleter>> conns;
		int input_buffer_size;
		int output_buffer_size;

		static void set_non_blocking_socket(int socket) {
#ifdef _MSC_VER
			unsigned long nbl = 1;
			ioctlsocket(socket, FIONBIO, &nbl);
#else
			int flags = fcntl(socket, F_GETFL);
			flags |= O_NONBLOCK;
			fcntl(socket, F_SETFL, flags);
#endif
		}

		static bool was_block() {
#ifdef _MSC_VER
			return (WSAGetLastError() == WSAEWOULDBLOCK);
#else
			return (errno == EAGAIN);
#endif
		}

#ifdef NET11_OVERLAPPED
		static void CALLBACK completion_input(DWORD err, DWORD count, WSAOVERLAPPED *ol, DWORD flags) {
			tcpconn *c = (tcpconn*)ol->hEvent;
			if (err == 0) {
				NET11_TCP_LOG("Pending read completed %x %d (overlap:%p)\n", c->sock, count,ol);
				if (count) {
					c->input.produced(count);
					c->ol_input_pending = false;
				} else {
					// a nil count means that we've finished the data stream, let's just ignore it to avoid touching sensitive data.
					c->want_input = false;
				}
			} else if (c) {
				NET11_TCP_LOG("Error on pending read %x\n", c->sock);
				c->want_input = false;
				c->ol_input_pending = false;
			} else {
				NET11_TCP_LOG("Error with invalid handle?\n");
			}
		}

		static void CALLBACK completion_output(DWORD err, DWORD count, WSAOVERLAPPED *ol, DWORD flags) {
			tcpconn *c = (tcpconn*)ol->hEvent;
			if (err == 0) {
				NET11_TCP_LOG("Ok on pending write %x %d (overlap:%p)\n", c->sock,count,ol);
				if (c->ol_output_pending && count) {
					c->output.consumed(count);
				}
				//if (count) {
				//	c->output.consumed(count);
				//} else {
				//	// flag problem on output write
				//}
				c->ol_output_pending = false;
			} else if (c) {
				NET11_TCP_LOG("Error on pending write %x\n", c->sock);
				c->ol_output_pending = false;
			} else {
				NET11_TCP_LOG("Error on invalid handle for write completion\n");
			}
		}

		bool work_conn(tcpconn &c) {
			int fill_count = 0;
			while (c.want_input && fill_count < 10) {
				// don't try to parse and produce more data if we have too much pending output.
				if (c.conn->producers.size() > 5)
					break;
				// as long as we still have input we will try to parse
				if (c.input.usage()) {
					NET11_TCP_LOG("Processing data on socket %x\n", c.sock);
					c.want_input = c.conn->current_sink->drain(c.input);
					c.want_input &= bool(c.conn->current_sink);
					continue;
				}
				// already an overlapped read in progress.
				if (c.ol_input_pending)
					break;
				// no input to read and no operation in progress, try or schedule a read.
				{
					memset(&c.overlapped_input, 0, sizeof(c.overlapped_input));
					c.overlapped_input.hEvent = &c;
					DWORD numRecv;
					DWORD flags = 0;
					c.wsa_input.len = c.input.compact();
					c.wsa_input.buf = c.input.to_produce();

					NET11_TCP_LOG("WSA Read on overlapped %p\n", &c.overlapped_input);
					int rr = WSARecv(c.sock, &(c.wsa_input), 1, &numRecv, &flags, &c.overlapped_input, completion_input);
					if (rr == 0) {
						// immediate completion
						NET11_TCP_LOG("Read immediate completion on %x %d\n", c.sock, numRecv);
						c.input.produced(numRecv);
						fill_count++;
						continue;
					} else if (rr == SOCKET_ERROR) {
						if (WSAGetLastError() == WSA_IO_PENDING) {
							// ok, pending read.
							c.ol_input_pending = true;
							NET11_TCP_LOG("Pending read on %x\n", c.sock);
							break;
						} else {
#ifdef NET11_VERBOSE
							char *msg;
							FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, nullptr);
							fprintf(stderr, "Unhandled error on read %s\n", msg);
							LocalFree(msg);
#endif
							// some error!!
							return false;
						}
					} else abort(); // invalid state
				}
			}
			// don't do any output processing while waiting for old data to be sent.
			while (!c.ol_output_pending) {
				//int precount=
				c.output.compact();
				// first fill the output buffer as much as possible.
				while (c.conn->producers.size()) {
					int preuse = c.output.total_avail();
					if (!c.conn->producers.front()(c.output)) {
						// producer finished, remove it.
						c.conn->producers.erase(c.conn->producers.begin());
					} else if (preuse == c.output.total_avail()) {
						// no data was generated.
						break;
					}
				}
				if (!c.output.usage())
					break;
				// then prepare a send
				memset(&c.overlapped_output, 0, sizeof(c.overlapped_output));
				c.overlapped_output.hEvent = &c;
				c.wsa_output.len = c.output.usage();
				c.wsa_output.buf = c.output.to_consume();
				DWORD outbytecount;
				NET11_TCP_LOG("** WSASend invoked %p\n",&c.overlapped_output);
				int wrv = WSASend(c.sock, &c.wsa_output, 1, &outbytecount, 0, &c.overlapped_output, completion_output);
				if (wrv == 0) {
					NET11_TCP_LOG("Wrote directly on socket %x %d\n", c.sock,outbytecount);
					c.output.consumed(outbytecount);
					continue;
				} else if (wrv==SOCKET_ERROR) {
					if (WSAGetLastError() == WSA_IO_PENDING) {
						NET11_TCP_LOG("Write pending on socket %x\n", c.sock);
						c.ol_output_pending = true;
						break;
					} else {
#ifdef NET11_VERBOSE
						char *msg;
						FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER, nullptr, WSAGetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), (LPSTR)&msg, 0, nullptr);
						fprintf(stderr, "Unhandled error on write %s\n", msg);
						LocalFree(msg);
#endif
						// some error!!
						return false;
					}
				} else abort();
			}
			return c.want_input || c.output.usage() || c.conn->producers.size() || c.ol_output_pending || c.ol_input_pending;
		}

#else
		bool work_conn(connection &c) {
			int fill_count = 0;
			while (c.want_input && fill_count<10) {
				// process events as long as we have data and don't have multiple
				// producers on queue to avoid denial of service scenarios where
				// the producers are too slow to drain before the sink has read.
				// Note: the sink should be called first since was_block below will break the loop on block.
				if (c.input.usage() && c.producers.size() <= 1) {
					c.want_input = c.current_sink->drain(c.input);
					c.want_input &= bool(c.current_sink);
					continue;
				}
				// try to fill up the buffer as much as possible.
				if (c.input.total_avail()) {
					int avail = c.input.compact();
					int rc = recv(c.sock, c.input.to_produce(), avail, 0);
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
			bool eop = false;
			while (c.output.usage() || c.producers.size()) {
				if (c.output.usage()) {
#ifdef _MSC_VER
					const int send_flags = 0;
#else
					const int send_flags = MSG_NOSIGNAL;
#endif
					int rc = send(c.sock, c.output.to_consume(), c.output.usage(), send_flags);
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
				if (eop)
					break;
				while (c.output.total_avail() && c.producers.size()) {
					int preuse = c.output.total_avail();
					if (!c.producers.front()(c.output)) {
						// producer finished, remove it.
						c.producers.erase(c.producers.begin());
					} else if (preuse == c.output.total_avail()) {
						// no data was generated.
						eop = true;
						break;
					}
				}
			}
			return c.want_input || c.output.usage() || c.producers.size();
		}
#endif


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
			//if (WSACleanup()) {
				//std::cerr<<"WSACleanup shutdown error"<<std::endl;
				//throw new std::exception("WSACleanup shutdown error\n");
			//}
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
#ifndef NET11_OVERLAPPED
						set_non_blocking_socket(newsock);
#endif
						//conns.push_back(std::shared_ptr<connection>(
						conns.emplace_back(
							new connection(input_buffer_size,output_buffer_size)
							//,[](auto p) { delete p; }
						);
						conns.back()->sock=newsock;
						conns.back()->want_input=true;
						
						//conns.back()->conn=l.second()->shared_from_this();
						//conns.back()->conn.reset(l.second());
						l.second(conns.back().get());
						
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
					[this](auto & c) {
						//printf("running conn:%p\n",&c);
						if (!work_conn(*c)) {
							NET11_TCP_LOG("Wanting to remove conn %x!\n",c->sock);
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
		bool listen(int port,std::function<void(connection*)> spawn) {
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
		bool connect(const std::string & host,int port,const std::function<void(connection*)> spawn) {
			struct sockaddr_in sockaddr;
			memset(&sockaddr,0,sizeof(sockaddr));
			sockaddr.sin_family=AF_INET;
			sockaddr.sin_port=htons(port);
			sockaddr.sin_addr.s_addr=inet_addr(host.c_str());
			if (sockaddr.sin_addr.s_addr==INADDR_NONE) {
				struct hostent *he=gethostbyname(host.c_str());
				if (!he) {
					printf("Failed getting hostname");
					return true;
				}
				memcpy(&sockaddr.sin_addr,he->h_addr,sizeof(sockaddr.sin_addr));
			}
			
			int sock=-1;
			if (-1==(sock=socket(PF_INET,SOCK_STREAM,IPPROTO_TCP))) {
				return true;
			}
			if (::connect(sock,(struct sockaddr*)&sockaddr,sizeof(sockaddr))) {
				printf("Connect failed?\n");
				closesocket(sock);
				return true;
			}
			set_non_blocking_socket(sock);
			//std::shared_ptr<connection> out(
			auto* out=new connection(input_buffer_size,output_buffer_size);
			//	[](auto p) { delete p; }
			//);
			out->sock=sock;
			out->want_input=true;
			spawn(out);
			//out->conn.reset(spawn());
			conns.emplace_back(out);
			return false;
		}

//		class connection : public std::enable_s {
//			connection() {
//				//printf("conn ctor\n");
//			}
//			virtual ~connection() {
//				//printf("conn dtor\n");
//			}
//		public:
//		};
	};
}

