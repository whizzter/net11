#include <functional>
#include <exception>
#include <memory>
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

namespace net11 {
	// the tcp class is a container for listeners and connections
	class tcp;
	// a single connection, protocols should inherit this class
	class connection;
	// a sink is a data receiver
	class sink;
	// buffer is a utility class built to pass along data as a memory fifo
	class buffer;

	// a utility sink that reads a full line
	class line_parser_sink;
	// a utility sink that parses RFC 822 headers
	class header_parser_sink;

	// utility functions to create producers from a string or vector
	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T * in_data);
	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T &in_data);

	// a utility function to give up a slice of cpu time
	void yield();

	class buffer {
		int m_cap;    // the total number of bytes in this buffer
		int m_bottom; // the bottom index, ie the first used data element
		int m_top;    // the top index, the first unused data element
		char *m_data; // the actual data
	public:
		buffer(int capacity) {
			m_cap=capacity;
			m_bottom=0;
			m_top=0;
			m_data=new char[capacity];
		}
		~buffer() {
			delete m_data;
		}
		// returns the number of bytes corrently in the buffer
		int usage() {
			return m_top-m_bottom;
		}
		// returns the number of bytes available to produce as a flat array
		int direct_avail() {
			return m_cap-m_top;
		}
		// returns the total number of bytes available to produce
		int total_avail() {
			return (m_cap-m_top)+(m_bottom);
		}
		// compacts the buffer to maximize the flatly available bytes
		int compact() {
			if (m_bottom==0)
				return direct_avail();
			int sz=usage();
			std::memmove(m_data,m_data+m_bottom,sz);
			m_bottom=0;
			m_top=sz;
			return direct_avail();
		}
		// consumes one byte from the currently available bytes
		char consume() {
			if (m_bottom>=m_top)
				throw std::out_of_range("no bytes to consume in buffer");
			return m_data[m_bottom++];
		}
		// returns the pointer to a number of bytes to consume directly.
		const char* to_consume() {
			return m_data+m_bottom;
		}
		// tells the buffer how many bytes was consumed
		void consumed(int amount) {
			if (usage()<amount || amount<0)
				throw std::invalid_argument("underflow");
			m_bottom+=amount;
		}
		// adds a byte to the buffer
		void produce(char c) {
			if (direct_avail()<1) {
				if (compact()<1) {
					throw std::out_of_range("no bytes available in buffer");
				}
			}
			m_data[m_top++]=c;
		}
		// adds as many bytes as possible from the source to this buffer
		void produce(buffer &source) {
			// how much do we want to copy if possible?
			int to_copy=source.usage();
			// now check the actual number of bytes we can copy
			if (to_copy>total_avail())
				to_copy=total_avail();
			produce(source,to_copy);
		}
		// copy the number of bytes from the source
		void produce(buffer &source,int to_copy) {
			// if we can fit it straight away copy it directly
			if (direct_avail()<to_copy) {
				// we can't copy it directly, then compact first before copy
				if (compact()<to_copy) {
					// still not enough space, fault here!
					throw std::invalid_argument("not enough space to take the copied bytes");
				}
			}
			// now copy the amount we can take
			std::memcpy(to_produce(),source.to_consume(),to_copy);
			produced(to_copy);
			source.consumed(to_copy);
		}
		// returns a the pointer to the free bytes to be written
		char* to_produce() {
			return m_data+m_top;
		}
		// tell the buffer how many bytes were actually written
		void produced(int amount) {
			if (direct_avail()<amount)
				throw std::invalid_argument("overflow");
			m_top+=amount;
		}
		// convert the buffer contents to a string
		std::string to_string() {
			std::string out;
			for (int i=m_bottom;i<m_top;i++) {
				out.push_back(m_data[i]);
			}
			return out;
		}
	};

	class sink {
	public:
		// implement this function to make a working sink
		virtual bool drain(buffer &buf)=0;
	};

	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T * in_data) {
		std::shared_ptr<int> off(new int);
		std::shared_ptr<T> data(in_data);
		*off=0;
		// return the actual producer function that writes out the contents on request
		return [data,off](buffer &ob){
			int dataleft=data->size()-*off; // dl is how much we have left to send
			int outleft=ob.compact();
			int to_copy=dataleft<outleft?dataleft:outleft;
			std::memcpy(ob.to_produce(),data->data()+*off,to_copy);
			ob.produced(to_copy);
			*off+=to_copy;
			return *off!=data->size();
		};
	}

	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T &in_data) {
		return make_data_producer(new T(in_data));
	}

	class line_parser_sink : public sink {
		std::string out;  // the output string
		const char *term; // the line terminator
		int tl;           // length of the terminator string
		int szmax;        // the maximum number of bytes in a line
		std::function<bool(std::string&)> on_line;
	public:
		line_parser_sink(
			const char *in_term,
			int in_max,
			std::function<bool(std::string&)> in_on_line
		):
			term(in_term),
			szmax(in_max),
			on_line(in_on_line)
		{
			tl=strlen(term);
		}
		virtual bool drain(buffer &buf) {
			size_t sz=out.size();
			while(buf.usage()) {
				if (sz>=szmax) {
					return 0;
				}
				//std::cout<<"Pre:["<<out<<"]["<<buf.to_string()<<"]\n";
				out.push_back(buf.consume());
				//std::cout<<"Post:["<<out<<"]["<<buf.to_string()<<"]\n";
				sz++;
				if (sz>tl) {
					if (!memcmp(out.data()+sz-tl,term,tl)) {
						out.resize(sz-tl);
						//std::cout<<"Line:"<<out<<":\n";
						bool rv=on_line(out);
						out.resize(0);
						return rv;
					}
				}
			}
			return this;
		}
	};

	class header_parser_sink : public sink {
		enum headerstate {
			firstlinestart=0,
			linestart,
			testemptyline,
			inkey,
			postkeyskip,
			invalue,
			postvalue
		};
		headerstate state;
		std::string k;
		std::string v;
		int count;
		int maxsz;

		std::function<bool(std::string&,std::string&)> on_header;
		std::function<bool(const char *err)> on_fin;
	public:
		header_parser_sink(
			int in_maxsz,
			std::function<bool(std::string&,std::string&)> in_on_header,
			std::function<bool(const char *err)> in_on_fin
		):
			state(firstlinestart),
			count(0),
			maxsz(in_maxsz),
			on_header(in_on_header),
			on_fin(in_on_fin)
		{}
		virtual bool drain(buffer &buf) {
			// pre-existing error condition, just return.
			if (count==-1)
				return false;
			while(buf.usage()) {
				if (count>=maxsz) {
					on_fin("Error, headers too large");
					count=-1;
					return false;
				}
				char c=buf.consume();
				count++;
				switch(state) {
				case firstlinestart :
				case linestart :
					if (c==13) {
						// empty line in progress
						state=testemptyline;
						continue;
					} else if (c==10) {
						on_fin("spurios LF");
						count=-1;
						return false;
					}
					if (state!=firstlinestart) {
						if (isspace(c)) {
							state=invalue;
							v.push_back(c);
							continue;
						}
						on_header(k,v);
						k.clear();
						v.clear();
					}
					if (isspace(c))
						continue;
					state=inkey;
					k.push_back(c);
					continue;
				case testemptyline :
					if (c==10) {
						// empty line encountered, we're finished with the data
						bool rv=on_fin(nullptr);
						k.clear();
						v.clear();
						state=firstlinestart;
						count=0;
						return rv;
					} else {
						on_fin("cr but no lf in empty headerline");
						count=-1;
						return false;
					}
				case inkey :
					if (c==':') {
						state=postkeyskip;
						continue;
					} else {
						k.push_back(c);
						continue;
					}
				case postkeyskip :
					if (isspace(c)) {
						continue;
					} else {
						state=invalue;
						v.push_back(c);
						continue;
					}
				case invalue :
					if (c==13) {
						state=postvalue;
						continue;
					} else {
						v.push_back(c);
						continue;
					}
				case postvalue :
					if (c==10) {
						state=linestart;
						continue;
					} else {
						on_fin("cr but no lf in headerline");
						count=-1;
						return false;
					}
				default:
					printf("headerparser unhandled state:%d\n",state);
					exit(-1);
				}
			}
			return true;
		}
	};

	class connection {
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

	class tcp {
		std::vector<std::pair<int,std::function<connection*()>>> listeners;
		class tcpconn {
			friend tcp;
			int sock;
			// unique_ptr is broken, ifdef?
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
			if (c.input.total_avail() && c.want_input) {
				int avail=c.input.compact();
				int rc=recv(c.sock,c.input.to_produce(),avail,0);
				if (rc<0) {
					if (was_block()) {
						//printf("WOULD BLOCK\n");
					} else {
						// not a blocking error!
						return false;
					}
				} else if (rc>0) {
					c.input.produced(rc);
					while(c.input.usage() && (c.want_input=c.conn->current_sink->drain(c.input))) {}
				} else {
					// 0 on recv, closed sock;
					return false;
				}
			}
			while (c.output.usage() || c.conn->producers.size()) {
				if (c.output.usage()) {
					int rc=send(c.sock,c.output.to_consume(),c.output.usage(),0);
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
					if (!c.conn->producers.front()(c.output)) {
						c.conn->producers.erase(c.conn->producers.begin());
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

