#include <string>
#include <map>
#include <algorithm>

#include <iostream>
#include <sys/stat.h>
#include <stdio.h>

#include "util.hpp"
#include "tcp.hpp"
#include "base64.hpp"
#include "sha1.hpp"

namespace net11 {
	namespace http {
		// The HTTP module of net11 is a state-machine built on sinks managing each state.

		// The state machine owner class that becomes the context of a tcp connection
		// Killing it will destroy references to the rest of the HTTP system
		class connection;

		// When a HTTP request is made the router produces actions on how to proceed
		// processing each request. All responses are actions but actions can also be
		// to read/process an input before responding (such as PUT,POST,etc) 
		class actiondata;
		class responsedata;
		class consume_action;
		using action=std::unique_ptr<actiondata>;
		using response=std::unique_ptr<responsedata>;

		// Websocket support is built in and initiated as a response
		class websocket_response;
		class websocket_sink;
		class websocket;

		// RFC 2616 sec2 Token
		bool parse_token_byte(std::string *out,buffer &in) {
			switch(in.peek()) {
			case -1 :
			case '(' : case ')' : case '<' : case '>' : case '@' :
			case ',' : case ';' : case ':' : case '\\' : case '\"' :
			case '/' : case '[' : case ']' : case '?' : case '=' :
			case '{' : case '}' : case ' ' : case '\t' :
				return false;
			}
			char v=in.consume();
			if (out)
				out->push_back(v);
			return true;
		}

		// RFC 2616 sec2 quoted-string (requires 3 consecutive state values)
		bool parse_quoted_string_byte(std::string *out,buffer &in, int &state,int base_state=0) {
			int lstate=state-base_state;
			if (lstate==0) {
				switch(in.peek()) {
				case -1 :
					return true;
				case '\"' :
					in.consume();
					state++;
					return true;
				default:
					return false;
				}
			} else if (lstate==1) {
				switch(in.peek()) {
				case -1 :
					return true;
				case '\\' :
					in.consume();
					state++;
					return true;
				case '\"' :
					in.consume();
					state--;
					return false;
				}
				char ov=in.consume();
				if (out)
					out->push_back(ov);
				return true;
			} else if (lstate==2) {
				char ov=in.consume();
				if (out)
					out->push_back(ov);
				state--;
				return true;
			}
			// this is an error state
			abort();
		}

		response make_text_response(int code,const std::string &data);

		// actiondata instances are implemention private and decides how the machine should proceed.
		class actiondata {
			friend class responsedata;
			friend class connection;
			friend class consume_action;
			actiondata() {}
		protected:
			virtual bool produce(connection &conn)=0;
		public:
			virtual ~actiondata() {}
		};

		// responses are a specialization of actions tht produce headers and output content
		class responsedata : public actiondata {
			friend class connection;
			friend class websocket_response;
			friend response make_stream_response(int code,std::function<bool(buffer &data)> prod);

			std::map<std::string,std::string> head;
			std::function<bool(buffer &)> prod;

			responsedata(){}
		protected:
			int code;

			// header helper producer (common for most responses)
			virtual void produce_headers(connection &conn);
			// could be specialized 
			virtual bool produce(connection &conn);

		public:
			void set_header(const std::string &k,const std::string &v) {
				head[k]=v;
			}
			virtual ~responsedata() {}
		};

		// the main HTTP connection managing class
		class connection {
			friend std::function<void(net11::tcp::connection*)> make_server(const std::function<action(connection &conn)>& route);
			friend response make_websocket(connection &c,int max_packet,std::function<bool(websocket &s,std::vector<char>&)> on_data,std::function<void()> on_close);
			friend class responsedata;
			friend class consume_action;
			friend class websocket;
			friend class websocket_response;

			// reference to the actual tcp connection that does input/output
			tcp::connection *tconn;

			// a weak this-ptr used to provide the shared ptr to things that needs a reference.
			std::weak_ptr<connection> wthis;

			// the reqlinesink parses request lines (finds method, url and http version data)
			std::shared_ptr<sink> reqlinesink;
			// contain the found data
			std::string reqline[3];

			// decides the next default request sink (could be overridden by HTTP-upgrades)
			std::shared_ptr<sink> nextreqsink() {
				// by default we work with keep-alive connections on HTTP/1.1
				// so we proceed to read another request.
				if (reqline[2].size() && reqline[2]=="HTTP/1.1") {
					return reqlinesink;
				} else {
					// on older HTTP versions we don't do more requests.
					return nullptr;
				}
			}

			// the header sink is responsible for parsing the http headers received.
			std::shared_ptr<sink> headsink;
			// the headers are stored here
			std::map<std::string,std::string> headers;

			std::shared_ptr<sink> postchunkedsink;
			std::shared_ptr<sink> m_chunkedcontentsink;

			// The router function, this function
			const std::function<action(connection &conn)> router;

			// remenant of older consumption code?
			// std::function<response*(buffer& data,bool end)> dataconsumer;

			// a function that is enabled by consume actions to pass over read data to
			// end consumers when the server is expecting data.
			std::function<response(buffer*data)> consume_fun;

			// only produce once per request
			bool produced;
			// actual function to invoke the requested production
			bool produce(action&& act);

			class sizedcontentsink : public sink {
				friend class connection;
				connection *conn;
				size_t clen;
				sizedcontentsink(connection *in_conn) : conn(in_conn),clen(0) {}
			public:
				virtual bool drain(buffer &buf) {
					bool rv=true;
					int amount=buf.usage()<clen?buf.usage():clen;
					if (conn->consume_fun)
					{
						buffer view(buf.to_consume(),amount);
						response r=conn->consume_fun(&view);
						if (r)
							rv=conn->produce((action)std::move(r));
					}
					buf.consumed(amount);
					clen-=amount;
					if (clen==0) {
						conn->tconn->current_sink=conn->nextreqsink();
						response r=0;
						if (conn->consume_fun) {
							r=conn->consume_fun(NULL);
						}
						bool rv=conn->produce(std::move(r));
						return rv;
					}
					return rv;
				}
			};

			class chunkedcontentsink : public sink {
				friend class connection;
				int state;
				int sstate;
				size_t clen;
				chunkedcontentsink() : state(0),clen(0),sstate(0) {}
				int hexcharvalue(int v) {
					if (v>='0' && v<='9')
						return v-'0';
					if (v>='a' && v<='f')
						return v-'a'+10;
					if (v>='A' && v<='F')
						return v-'A'+10;
					return -1;
				}
				connection *conn;
				chunkedcontentsink(connection *in_conn) : conn(in_conn),state(0),sstate(0),clen(0) {}
			public:
				virtual bool drain(buffer &buf) {
					bool rv=true;
					while(buf.usage()) {
						int cv=buf.peek();
						switch(state) {
						case 0 : // parsing chunk size
							{
								int hv=hexcharvalue(cv);
								if (hv==-1) { // check if it wasn't a hex-char
									state=1;  // decide on ext or content
									continue;
								}
								buf.consume();
								clen=clen*16 + hv;
								continue;
							}
						case 1 : // post size/ext, decide on next action
							buf.consume();
							if (cv==';') {
								state=5; // expect ext-name
								continue;
							} else if (cv==13) {
								state=2; // expect LF
								continue;
							} else return false; // syntax error here
						case 2 : // chunk-LF
							buf.consume(); // always consume
							if (cv!=10)
								return false; // syntax error
							if (clen==0) {
								state=0; sstate=0; clen=0;
								conn->tconn->current_sink=conn->postchunkedsink;
								return rv;
							} else {
								state=9; // content
								continue;
							}
						case 3 : // content-CR
							buf.consume();
							if (cv!=13)
								return false; // syntax error
							state=4;
							continue;
						case 4 : // content-LF
							buf.consume();
							if (cv!=10)
								return false; // syntax error
							state=0; // go back to reading a chunk-size
							continue;
						case 5 : // ext-name
							if (parse_token_byte(NULL,buf)) // consume as much of a name as possible
								continue;
							// but after the name token we have something else.
							cv=buf.consume();
							if (cv=='=') {
								// either require a value
								state=6;
								continue;
							} else if (cv==13) {
								// or a CRLF to go to the content
								state=2;
								continue;
							} else return false; // syntax error otherwise
						case 6 : // start of ext value, could be...
							if (cv=='\"') {
								state=8; // a quoted string
								continue;
							} else {
								state=7; // or a simple token
								continue;
							}
						case 7 : // parsing token ext value
							if (parse_token_byte(NULL,buf))
								continue;
							state=1; // decide on what to do next
							continue;
						case 8 : // parsing quoted string ext value
							if (parse_quoted_string_byte(NULL,buf,sstate))
								continue;
							state=1; // decide on what to do next
							continue;
						case 9 : // X bytes of content
							{
								int amount=buf.usage()<clen?buf.usage():clen;
								if (conn->consume_fun)
								{
									buffer view(buf.to_consume(),amount);
									response r=conn->consume_fun(&view);
									if (r)
										rv=conn->produce(std::move(r));
								}
								buf.consumed(amount);
								clen-=amount;
								if (clen==0) {
									state=3;
								}
								continue;
							}
						default: // illegal state
							abort();
						}
					}
					return rv;
				}
			};

			std::shared_ptr<sizedcontentsink> m_sizedcontentsink;

			connection(
				tcp::connection* tcp_conn,
				const std::function<action(connection &conn)>& in_router
			):tconn(tcp_conn),router(in_router) {
				reqlinesink=std::shared_ptr<sink>(new net11::line_parser_sink("\r\n",4096,[this](std::string &l){
					bool in_white=false;
					int outidx=0;
					reqline[0].resize(0);
					reqline[1].resize(0);
					reqline[2].resize(0);
					headers.clear();
					produced=false;
					for (int i=0;i<l.size();i++) {
						char c=l[i];
						if (isspace(c)) {
							in_white=true;
						} else {
							if (in_white && outidx<2) {
								outidx++;
							}
							reqline[outidx].push_back(c);
							in_white=false;
						}
					}
					if (outidx>0 && reqline[0].size() && reqline[1].size()) {
						this->tconn->current_sink=headsink;
						return true;
					} else {
						// TODO: error handling?!
						return false;
					}
				}));
				headsink=std::shared_ptr<sink>(new header_parser_sink(128*1024,tolower,
					[this](std::string &k,std::string &v){
#ifdef NET11_VERBOSE
						std::cout<<"HeadKey=["<<k<<"] HeadValue=["<<v<<"]\n";
#endif
						headers[k]=v;
						return true;
					},
					[this](const char *err) {
#ifdef NET11_VERBOSE
						std::cout<<"req:"<<reqline[0]<<" url:"<<reqline[1]<<" ver:"<<reqline[2]<<"\n";
#endif
						consume_fun=nullptr;
						// reset our sink early in case the encoding, router and/or action wants to hijack it
						// determine content based on RFC 2616 pt 4.4
						auto tehead=this->header("transfer-encoding");
						if ( tehead && *tehead!="identity" ) {
							// Chunked encoding if transfer-encoding header exists and isn't set to identity
							//std::cerr<<"CHUNKED!\n";
							this->tconn->current_sink=m_chunkedcontentsink;
						} else if (auto clhead=this->header("content-length")) {
							//std::cerr<<"Content LEN\n";
							// only allow content-length influence IFF no transfer-enc is present
							net11::trim(*clhead);
							size_t clen=std::stoi(*clhead);
							m_sizedcontentsink->clen=clen>=0?clen:0;
							this->tconn->current_sink=m_sizedcontentsink;
						} else {
							// this server doesn't handle other kinds of content
							this->tconn->current_sink=nextreqsink();
						}
						// TODO: urlencodings?
						action act=router(*this);
						bool rv=produce(std::move(act));
						return rv;
					}
				));
				m_chunkedcontentsink=std::shared_ptr<sink>(new chunkedcontentsink(this));
				m_sizedcontentsink=std::shared_ptr<sizedcontentsink>(new sizedcontentsink(this));
				postchunkedsink=std::shared_ptr<sink>(new header_parser_sink(128*1024,tolower,
					[this](std::string &k,std::string &v) {
						headers[k]=v;
						return true;
					},
					[this](const char *err){
						response r=0;
						if (consume_fun) {
							r=consume_fun(NULL);
						}
						this->tconn->current_sink=nextreqsink();
						bool rv=produce(std::move(r));
						return rv;
					}
				));
				tconn->current_sink=reqlinesink;
			}
			virtual ~connection() {
				//printf("Killed http connection\n");
			}
			// small helper for the template expansion of has_headers
			bool has_headers() {
				return true;
			}
		
		public:
			std::string& method() {
				return reqline[0];
			}
			std::string& url() {
				return reqline[1]; // TODO should it be pre-decoded?
			}
			std::string* header(const char *in_k) {
				std::string k(in_k);
				return header(k);
			}
			std::string* header(std::string &k) {
				auto f=headers.find(k);
				if (f!=headers.end())
					return &f->second;
				else
					return 0;
			}
			std::unique_ptr<std::pair<std::string,std::string>> get_basic_auth() {
				//auto bad=std::make_pair(std::string(""),std::string(""));
				auto authhead=header("authorization");
				if (!authhead)
					return nullptr;
				//std::cerr<<"To split:"<<(*authhead)<<std::endl;
				auto authsep=net11::split(*authhead,' ');
				//std::cerr<<"Type:"<<authsep.first<<" Val:"<<authsep.second<<std::endl;
				net11::trim(authsep.first);
				// only support basic auth right now
				if (net11::stricmp(authsep.first,"basic"))
					return nullptr;
				//std::cerr<<"To decode:"<<authsep.second<<std::endl;
				net11::trim(authsep.second);
				auto tmp=net11::base64decoder().decode(authsep.second);
				//std::cerr<<"Decoded auth:"<<tmp<<std::endl;
				return std::unique_ptr<std::pair<std::string,std::string>>(
					new std::pair<std::string,std::string>(net11::split(tmp,':'))
				);
			}
			std::string lowerheader(const char *k) {
				std::string ok(k);
				return lowerheader(ok);
			}
			std::string lowerheader(std::string &k) {
				std::string out;
				auto f=headers.find(k);
				if (f!=headers.end()) {
					for (int i=0;i<f->second.size();i++)
						out.push_back(tolower(f->second[i]));
				}
				return out;
			}
			template<class T>
			void csvheaders(const std::string &k,T& fn,bool param_tolower=false) {
				auto f=headers.find(k);
				if (f==headers.end())
					return;
				std::string h;
				auto & is=f->second;
				for(int i=0;i<=is.size();) {
					bool end=i==is.size();
					if (!end && !h.size() && isspace(is[i])) {
						i++;
						continue;
					}
					if (end || is[i]==',') {
						if (h.size())
							fn(h);
						h.clear();
						i++;
						continue;
					}
					if (param_tolower) {
						h+=tolower(is[i++]);
					} else {
						h+=is[i++];
					}
				}
			}
			bool has_header(std::string &k) {
				return headers.count(k)!=0;
			}
			bool has_header(const char *p) {
#ifdef NET11_VERBOSE
				using namespace std::literals;
				std::cout
					<<"HasHeader:"
					<<p
					<<" -> "
					<<(headers.count(std::string(p))!=0)
					<<" "
					<<((headers.count(std::string(p))!=0)?("[[["+lowerheader(p)+"]]]"): ""s)
					<<"\n";
#endif
				return headers.count(std::string(p))!=0;
			}
			template<typename HEAD,typename... REST>
			bool has_headers(HEAD head,REST... rest) {
				return has_header(head)&&has_headers(rest...);
			}
		};

		std::function<void(net11::tcp::connection*)> make_server(const std::function<action(connection &conn)>& route) {
			// now create a connection spawn function
			return [route](net11::tcp::connection* tconn) {
				std::shared_ptr<connection> conn(
					new connection(tconn,route),
					[](auto p) { delete p; }
				);
				conn->wthis=conn;
				tconn->ctx=conn;
			};
		};

		bool start_server(net11::tcp& l,int port,const std::function<action(connection&conn)>& route) {
			return l.listen(port,make_server(route));
		}

		class consume_action : public action {
		protected:
			std::function<response(buffer*buf)> fn;
			virtual bool produce(connection &conn) {
				//std::cerr<<"Nil production right now from consume action\n";
				conn.consume_fun=fn;
				return true;
			}
		public:
			consume_action(const std::function<response(buffer *buf)> & in_fn) : fn(in_fn) {}
			~consume_action(){}
		};

		response make_stream_response(int code,std::function<bool(buffer &data)> prod) {
			//auto out=new response();
			response out(new responsedata()); //,[](auto p){delete p;} );
			out->code=code;
			out->prod=prod;
			return out;
		}

		response make_blob_response(int code,const std::vector<char> &in_data) {
			auto rv=make_stream_response(code,make_data_producer(in_data));
			rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
			return rv;
		}
		response make_text_response(int code,const std::string &in_data) {
			auto rv=make_stream_response(code,make_data_producer(in_data));
			rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
			return rv;
		}

		class websocket : public std::enable_shared_from_this<websocket> {
			friend websocket_sink;
			std::weak_ptr<connection> conn;
			int input_type=-1;
			websocket(std::weak_ptr<connection> in_conn):conn(in_conn),input_type(-1) {}
		public:
			int get_input_type() {
				return input_type;
			}
			static const int text=1;
			static const int binary=2;
			bool send(int ty,const char *data,uint64_t sz) {
				if (auto c=conn.lock()) {
					int shift;
					int firstsize;
					if (sz<126) {
						shift=0;
						firstsize=sz;
					} else if (sz<65536) {
						shift=16;
						firstsize=126;
					} else {
						shift=64;
						firstsize=127;
					}
					std::shared_ptr<buffer> b(new buffer(2+(shift/8)+sz));
					b->produce(0x80|ty);
					b->produce(firstsize);
					while(shift) {
						shift-=8;
						b->produce( (sz>>shift)&0xff );
					}
					std::memcpy(b->to_produce(),data,sz);
					b->produced(sz);
					c->tconn->producers.push_back([b](buffer& out) {
						out.produce(*b);
						return 0!=b->usage();
					});
					return true;
				} else {
					// Sending to killed connection!
					return false;
				}
			}
			bool send(const std::string& data) {
				return send(text,data.data(),data.size());
			}
			bool send(const std::vector<char>& data) {
				return send(binary,data.data(),data.size());
			}
		};

		class websocket_sink : public sink {
			friend websocket_response;
			enum wsstate {
				firstbyte=0,
				sizebyte,
				sizeextra,
				maskbytes,
				bodybytes
			};
			wsstate state;
			uint8_t info;
			uint64_t count;
			uint64_t size;
			bool want_mask;
			uint32_t mask;
			//std::vector<char> data;
			
			char control_data[125];
			
			bool endit() {
				bool fin=info&0x80;
				if ((info&0xf)<=2) {
					int type=info&0xf;
					if (websock->input_type==-1) {
						if (type==0)
							return false; // must get a new type!
						websock->input_type=type;
					} else {
						if (type!=0)
							return false; // already a set type!
					}
					if (!packet_end(fin,info&0xf))
						return false;
					if (fin)
						websock->input_type=-1;
				} else {
					// processing for non-data packets.
					if ((info&0xf)==8) {
						websock->send(8,control_data,0);
						return false;
					} else if ((info&0xf)==9) {
						// got a ping, need to do pong
						websock->send(10,control_data,size);
					} else if ((info&0xf)==10) {
						// 10, we ignore pongs..
					} else {
						// unknown packet type
						return false;
					}
				}
				state=firstbyte;
				return true;
			}
			bool advance() {
				count=0;
				if (state==sizebyte || state==sizeextra) {
					if (want_mask) {
						state=maskbytes;
						return true;
					}
				}
				state=bodybytes;
				bool ok=true;
				if (info&0x70)
					return false; // Not allowed to use reserved bits
				if (!(info&0x80) && ((info&0xf)>7))
					return false;
				if ((info&0xf)<=2) {
					ok&=packet_start(info&0x80,info&0xf,size);
				} else {
					if (size>sizeof(control_data)) {
						return false;
					}
					switch(info&0xf) {
					case 8 : // close
						ok=true;
						break;
					case 9 : case 10 : // ping-pong
						ok=true;
						break;
					default:
						ok=false; // do not know how to handle packet
						break;
					}
				}
				if (size==0) {
					return ok&endit();
				} else {
					return ok;
				}
			}
			std::shared_ptr<websocket> websock;
		protected:
			websocket_sink(std::weak_ptr<connection> conn):state(firstbyte),websock(new websocket(conn)) {}
		public:
			virtual bool drain(buffer &buf) {
				while(buf.usage()) {
					switch(state) {
					case firstbyte :
						info=buf.consume();
						state=sizebyte;
						mask=0;
						count=0;
						continue;
					case sizebyte :
						{
							int tmp=buf.consume();
							want_mask=tmp&0x80;
							if ((tmp&0x7f)<126) {
								size=tmp&0x7f;
								if (!advance())
									return false;
								continue;
							} else {
								if((tmp&0x7f)==126) {
									count=6; // skip "initial" 6 bytes since we only want a 2 byte size
								} else {
									count=0;
								}
								size=0;
								state=sizeextra;
								continue;
							}
						}
					case sizeextra:
						size=(size<<8)|(buf.consume()&0xff);
						if ((++count)==8) {
							if (!advance())
								return false;
						}
						continue;
					case maskbytes :
						mask=(mask<<8)|(buf.consume()&0xff);
						if ((++count)==4) {
							if (!advance())
								return false;
						}
						continue;
					case bodybytes :
						{
							int b=(buf.consume()^( mask >> ( 8*(3^(count&3))) ))&0xff;
							if ((info&0xf)<=2) {
								packet_data(b);
							} else {
								control_data[count]=b;
							}
							if (++count==size) {
								if (!endit())
									return false;
							}
							continue;
						}
					}
				}
				return true;
			}
			std::weak_ptr<websocket> get_websocket() {
				return websock;
			}
			virtual bool packet_start(bool fin,int type,uint64_t size)=0;
			virtual void packet_data(char c)=0;
			virtual bool packet_end(bool fin,int type)=0;
			virtual void websocket_closing() {}
		};

		class websocket_response : public responsedata {
			friend response make_websocket(connection &c,std::shared_ptr<websocket_sink> wssink);
			std::shared_ptr<websocket_sink> sink;
			websocket_response(std::shared_ptr<websocket_sink> in_sink):sink(in_sink) {
				code=101;
			}
			bool produce(connection &conn) {
				produce_headers(conn);
				conn.tconn->current_sink=sink;
				return true;
			}
		public:
			std::weak_ptr<websocket> get_websocket() {
				return sink->websock;
			}
		};

		response make_websocket(connection &c,std::shared_ptr<websocket_sink> wssink) {
			bool has_heads=c.has_headers(
				"connection",
				"upgrade",
				//"origin",
				"sec-websocket-version",
				"sec-websocket-key");
			//printf("Has heads?:%d\n",has_heads);
			if (!has_heads)
				return 0;
			bool has_upgrade=false;
			c.csvheaders("connection",[&](auto& hval){ if (hval=="upgrade") has_upgrade=true; },true);
			if (!has_upgrade) {
				return 0;
			}
			if (c.lowerheader("upgrade")!="websocket") {
				return 0;
			}
			if (*c.header("sec-websocket-version")!="13") {
				return 0;
			}
			// hash the key and guid to know the response hash
			net11::sha1 s;
			char hash[20];
			s.addbytes(*c.header("sec-websocket-key"));
			s.addbytes("258EAFA5-E914-47DA-95CA-C5AB0DC85B11",36);
			s.digest(hash);
			// base64 encode the hash into a response token
			std::string rkey;
			net11::base64encoder().encode(rkey,hash,20,true);
			// now setup the response!
			websocket_response *ws=new websocket_response(wssink);
			ws->set_header("Upgrade","websocket");
			ws->set_header("Connection","upgrade");
			ws->set_header("Sec-Websocket-Accept",rkey);
			//ws.set_header("sec-websocket-protocol") // proto?!
			return response(ws);
		}

		response make_websocket(connection &c,int max_packet,std::function<bool(websocket &s,std::vector<char>&)> on_data,std::function<void()> on_close=std::function<void()>()) {
			struct websocket_packet_sink : public websocket_sink {
				int max_packet;
				std::vector<char> data;
				std::function<bool(websocket &s,std::vector<char>&)> on_data;
				std::function<void()> on_close;
				websocket_packet_sink(
					std::weak_ptr<connection> c,
					int in_max_packet,
					std::function<bool(websocket &s,std::vector<char>&)> in_on_data,
					std::function<void()> in_on_close)
				:websocket_sink(c),max_packet(in_max_packet),on_data(in_on_data),on_close(in_on_close) {
				}
				bool packet_start(bool fin,int type,uint64_t size) {
					if ((data.size()+size)>max_packet)
						return false;
					return true;
				}
				void packet_data(char b) {
					data.push_back(b);
				}
				bool packet_end(bool fin,int type) {
					std::shared_ptr<websocket> ws(get_websocket());
					bool ok=true;
					if (fin) {
						on_data(*ws,data);
						data.clear();
					}
					return ok;
				}
				void websocket_closing() {
					if (on_close) {
						on_close();
						on_close=std::function<void()>();
					}
				}
			};
			//std::shared_ptr<connection> sc=c.wthis; //=std::static_pointer_cast<connection>(c.shared_from_this());
			std::shared_ptr<websocket_packet_sink> sink(
				new websocket_packet_sink(c.wthis,max_packet,on_data,on_close)
			);
			return make_websocket(c,sink);
		}

		action match_file(connection &c,std::string urlprefix,std::string filepath) {
			if (0!=c.url().find(urlprefix))
				return 0; // not matching the prefix.
			std::string checked=c.url().substr(urlprefix.size());
			struct stat stbuf;
			int last='/';
			int end=checked.size();
			for (int i=0;i<checked.size();i++) {
				if (checked[i]=='\\')
					return make_text_response(500,"Bad request, \\ not allowed in url");
				if (checked[i]=='?') {
					end=i;
					break;
				}
				if (last=='/') {
					if (checked[i]=='.') {
						return 0;  // an error should be returned but could be an information leakage.
					} else if (checked[i]=='/') {
						return 0;  // an error should be returned but could be an information leakage.
					}
				}
				if (checked[i]=='/') {
					// check for directory presence!
					std::string tmp=filepath+checked.substr(0,i);
					int sr=stat( tmp.c_str(),&stbuf);
					if (sr) {
						return 0;
					}
					if (!(stbuf.st_mode&S_IFDIR)) {
						return 0;
					}
				}
				last=checked[i];
			}
			std::string tmp=filepath+checked.substr(0,end);
			if (stat(tmp.c_str(),&stbuf)) {
				return 0;
			}
			if (!(stbuf.st_mode&S_IFREG)) {
				return 0;
			}

			FILE *f=fopen(tmp.c_str(),"rb");
			if (!f)
				return 0;
			struct fh {
				FILE *f;
				fh(FILE *in_f):f(in_f){}
				~fh() {
					fclose(f);
				}
			};
			std::shared_ptr<fh> fp(new fh(f));

			auto out=make_stream_response(200,[fp](buffer &ob) {
				int osz=ob.usage();
				int tr=ob.compact();
				int rc=fread(ob.to_produce(),1,tr,fp->f);
				if (rc>=0) {
					ob.produced(rc);
				}
				if (rc<=0) {
					//if (feof(fp->f))
					return false; // always stop sending on error
				}
				return true;
			});
			out->set_header("content-length",std::to_string(stbuf.st_size));
			return out;
		}


			inline bool connection::produce(action&& act) {
				if (produced)
					return true;
				consume_fun=nullptr;
				if (!act) {
					//std::map<std::string,std::string> head; //{{"connection","close"}};
					std::string msg="Error 404, "+url()+" not found";
					act=(action)make_text_response(404,msg);
				}
				// TODO: make sure that connection lines are there?
				bool rv=act->produce(*this);
				// delete act; no longer needed..
				return rv;
			}

			inline void responsedata::produce_headers(connection &conn) {
				conn.produced=true;
				std::string resline="HTTP/1.1 "+std::to_string(code)+" OK\r\n";
				for(auto kv:head) {
					resline=resline+kv.first+": "+kv.second+"\r\n";
				}
				resline+="\r\n";
				conn.tconn->producers.push_back(make_data_producer(resline));
			}

			inline bool responsedata::produce(connection &conn) {
				produce_headers(conn);
				if (head.count("content-length")) {
					conn.tconn->producers.push_back(prod);
				} else {
					// TODO: implement chunked responses?
					abort(); //conn.producers.push_back([this](
				}
				return true;
			}


	}
}

