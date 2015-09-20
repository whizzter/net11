#include <regex>
#include <map>
#include <algorithm>

#include "tcp.hpp"

namespace net11 {
	class http_connection;
	class http_response;
	class http_websocket;

	http_response* make_text_response(int code,std::map<std::string,std::string> &head,std::string &data);

	class http_response {
		friend http_connection;
		friend http_response* make_stream_response(int code,std::map<std::string,std::string> &head,std::function<bool(std::vector<char> &data)> prod);

		int code;
		std::map<std::string,std::string> head;
		std::function<bool(std::vector<char> &)> prod;

		http_response(){}
	protected:
		virtual void produce(http_connection &conn);
	public:
		void set_header(std::string &k,std::string &v) {
			head[k]=v;
		}
		virtual ~http_response() {}
	};

	class http_connection : public net11::connection {
		friend std::function<net11::connection*()> make_http_server(std::function<http_response*(http_connection &conn)> route);
		friend http_response;
		std::function<bool(std::vector<char>&)> reqlinesink;
		std::shared_ptr<std::regex> rereqline;
		std::string reqline[3];

		std::function<bool(std::vector<char>&)> headsink;
		std::map<std::string,std::string> headers;

		std::function<http_response*(http_connection &conn)> router;

		http_connection(
			std::shared_ptr<std::regex> in_re,
			std::function<http_response*(http_connection &conn)> in_router
		):rereqline(in_re),router(in_router) {
			sink=reqlinesink=net11::make_line_sink("\r\n",4096,[this](std::string &l){
				std::smatch sma;
				if (std::regex_match(l,sma,*rereqline)) {
					reqline[0]=sma.str(1);
					reqline[1]=sma.str(2);
					if (sma.size()<3) {
						reqline[2]="";
					} else {
						reqline[2]=sma.str(3);
					}
					sink=headsink;
					return true;
				} else {
					// TODO: error handling?!
					return false;
				}
			});
			// TODO: add a max-headers-size parameter to replace the string sizes?
			headsink=net11::make_header_sink(80,65536,
				[this](std::string &k,std::string &v){
					//std::cout<<"HeadKey=["<<k<<"] HeadValue=["<<v<<"]\n";
					headers[k]=v;
					return true;
				},
				[this](const char *err) {
					//std::cout<<"req:"<<reqline[0]<<" url:"<<reqline[1]<<" ver:"<<reqline[2]<<"\n";
					//conn->sink=[](std)
					http_response *r=router(*this);
					if (!r) {
						std::map<std::string,std::string> head{{"connection","close"}};
						r=make_text_response(404,head,std::string("Error 404, not found"));
					}
					// TODO: make sure that connection lines are there?
					r->produce(*this);
					delete r;
					reqline[0].resize(0);
					reqline[1].resize(0);
					reqline[2].resize(0);
					headers.clear();
					return false;
				}
			);
		}
		virtual ~http_connection() {
			//printf("destroying http_connection\n");
		}
	public:
		std::string& method() {
			return reqline[0];
		}
		std::string& url() {
			return reqline[1]; // TODO should it be pre-decoded?
		}
		std::string* header(std::string &k) {
			auto f=headers.find(k);
			if (f!=headers.end())
				return &f->second;
			else
				return 0;
		}
	};

	std::function<net11::connection*()> make_http_server(std::function<http_response*(http_connection &conn)> route) {
		// init the regexp once if the regex compiler is slow.
		std::shared_ptr<std::regex> re(new std::regex("(\\S+)\\s+(\\S+)(?:\\s+(.+))?"));

		// now create a connection spawn function
		return [re,route]() {
			return (net11::connection*)new http_connection(re,route);
		};
	};

	http_response* make_stream_response(int code,std::map<std::string,std::string> &head,std::function<bool(std::vector<char> &data)> prod) {
		auto out=new http_response();
		out->code=code;
		out->head=head;
		out->prod=prod;
		return out;
	}
	http_response* make_blob_response(int code,std::map<std::string,std::string> &head,std::vector<char> &in_data) {
		auto rv=make_stream_response(code,head,make_data_producer(in_data));
		rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
		return rv;
	}
	http_response* make_text_response(int code,std::map<std::string,std::string> &head,std::string &in_data) {
		auto rv=make_stream_response(code,head,make_data_producer(in_data));
		rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
		return rv;
	}
	
	void http_response::produce(http_connection &conn) {
		std::string resline="HTTP/1.1 "+std::to_string(code)+" some message\r\n";
		for(auto kv:head) {
			resline=resline+kv.first+": "+kv.second+"\r\n";
		}
		resline+="\r\n";
		//std::cout<<"RESP:["<<resline<<"]";
		conn.producers.push_back(make_data_producer(resline));
		conn.producers.push_back(prod);
	}
}

