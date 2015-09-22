#include <regex>
#include <map>
#include <algorithm>

#include <sys/stat.h>
#include <fstream>

#include "tcp.hpp"

namespace net11 {
	class http_connection;
	class http_response;
	class http_websocket;

	http_response* make_text_response(int code,std::map<std::string,std::string> &head,std::string &data);


	class http_response {
		friend http_connection;
		friend http_response* make_stream_response(int code,std::map<std::string,std::string> &head,std::function<bool(buffer &data)> prod);

		int code;
		std::map<std::string,std::string> head;
		std::function<bool(buffer &)> prod;

		http_response(){}
	protected:
		virtual bool produce(http_connection &conn);
	public:
		void set_header(std::string &k,std::string &v) {
			for (int i=0;i<k.size();i++) {
				if (isupper(k[i])) {
					std::string k2;
					for (i=0;i<k.size();i++)
						k2.push_back(tolower(k[i]));
					head[k2]=v;
					return;
				}
			}
			head[k]=v;
		}
		virtual ~http_response() {}
	};

	class http_connection : public net11::connection {
		friend std::function<net11::connection*()> make_http_server(std::function<http_response*(http_connection &conn)> route);
		friend http_response;
		std::shared_ptr<sink> reqlinesink;
		std::shared_ptr<std::regex> rereqline;
		std::string reqline[3];

		std::shared_ptr<sink> headsink;
		std::map<std::string,std::string> headers;

		std::function<http_response*(http_connection &conn)> router;

		http_connection(
			std::shared_ptr<std::regex> in_re,
			std::function<http_response*(http_connection &conn)> in_router
		):rereqline(in_re),router(in_router) {
			reqlinesink=std::shared_ptr<sink>(new net11::line_parser_sink("\r\n",4096,[this](std::string &l){
				std::smatch sma;
				//std::cout<<"REqline:"<<l<<"\n";
				if (std::regex_match(l,sma,*rereqline)) {
					reqline[0]=sma.str(1);
					reqline[1]=sma.str(2);
					if (sma.size()<3) {
						reqline[2]="";
					} else {
						reqline[2]=sma.str(3);
					}
					this->current_sink=headsink;
					return true;
				} else {
					// TODO: error handling?!
					return false;
				}
			}));
			headsink=std::shared_ptr<sink>(new header_parser_sink(128*1024,
				[this](std::string &k,std::string &v){
					//std::cout<<"HeadKey=["<<k<<"] HeadValue=["<<v<<"]\n";
					headers[k]=v;
					return true;
				},
				[this](const char *err) {
					//std::cout<<"req:"<<reqline[0]<<" url:"<<reqline[1]<<" ver:"<<reqline[2]<<"\n";

					// TODO: urlencodings?
					http_response *r=router(*this);
					if (!r) {
						std::map<std::string,std::string> head; //{{"connection","close"}};
						r=make_text_response(404,head,"Error 404, "+url()+" not found");
					}
					// reset our sink in case the response wants to hijack it
					this->current_sink=reqlinesink;
					// TODO: make sure that connection lines are there?
					bool rv=r->produce(*this);
					delete r;
					reqline[0].resize(0);
					reqline[1].resize(0);
					reqline[2].resize(0);
					headers.clear();
					return rv;
				}
			));
			current_sink=reqlinesink;
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

	http_response* make_stream_response(int code,std::map<std::string,std::string> &head,std::function<bool(buffer &data)> prod) {
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
	http_response* make_blob_response(int code,std::map<std::string,std::string> head,std::vector<char> &in_data) {
		auto rv=make_stream_response(code,head,make_data_producer(in_data));
		rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
		return rv;
	}
	http_response* make_text_response(int code,std::map<std::string,std::string> &head,std::string &in_data) {
		auto rv=make_stream_response(code,head,make_data_producer(in_data));
		rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
		return rv;
	}
	http_response* make_text_response(int code,std::map<std::string,std::string> head,std::string in_data) {
		auto rv=make_stream_response(code,head,make_data_producer(in_data));
		rv->set_header(std::string("content-length"),std::to_string(in_data.size()));
		return rv;
	}

	bool http_response::produce(http_connection &conn) {
		std::string resline="HTTP/1.1 "+std::to_string(code)+" some message\r\n";
		for(auto kv:head) {
			resline=resline+kv.first+": "+kv.second+"\r\n";
		}
		resline+="\r\n";
		conn.producers.push_back(make_data_producer(resline));
		if (head.count("content-length")) {
			conn.producers.push_back(prod);
		} else {
			abort(); //conn.producers.push_back([this](
		}
		return true;
	}

	http_response* make_file_response(std::string base,std::string checked) {
		struct stat stbuf;
		int last='/';
		int end=checked.size();
		for (int i=0;i<checked.size();i++) {
			if (checked[i]=='\\')
				return make_text_response(500,{},"Bad request, \\ not allowed in url");
			if (checked[i]=='?') {
				end=i;
				break;
			}
			if (last=='/') {
				if (checked[i]=='.') {
					return make_text_response(500,{},"Bad request with a dotfile named file comming after a /");
				} else if (checked[i]=='/') {
					return make_text_response(500,{},"Bad request, multiple // after eachother");
				}
			}
			if (checked[i]=='/') {
				// check for directory presence!
				std::string tmp=base+checked.substr(0,i);
				int sr=stat( tmp.c_str(),&stbuf);
				if (sr) {
					return make_text_response(404,{},"Not found "+checked.substr(0,i));
				}
				if (!(stbuf.st_mode&S_IFDIR)) {
					return make_text_response(404,{},"Not a dir "+checked.substr(0,i));
				}
			}
			last=checked[i];
		}
		std::string tmp=base+checked.substr(0,end);
		if (stat(tmp.c_str(),&stbuf)) {
			return make_text_response(404,{},"Not found "+checked.substr(0,end));
		}
		if (!(stbuf.st_mode&S_IFREG)) {
			return make_text_response(404,{},"Not a file "+checked.substr(0,end));
		}
		
		FILE *f=fopen(tmp.c_str(),"rb");
		if (!f)
			return make_text_response(400,{},"Could not open file "+checked.substr(0,end));
		struct fh {
			FILE *f;
			fh(FILE *in_f):f(in_f){}
			~fh() {
				fclose(f);
			}
		};
		std::shared_ptr<fh> fp(new fh(f));

		std::map<std::string,std::string> head={
			{"content-length",std::to_string(stbuf.st_size)}
		};
		return make_stream_response(200,head,[fp](buffer &ob) {
			int osz=ob.usage();
			int tr=ob.compact();
			int rc=fread(ob.to_produce(),1,tr,fp->f);
			if (rc>=0) {
				ob.produced(osz+rc);
			}
			if (rc<=0) {
				if (feof(fp->f))
					return false;
			}
			return true;
		});
	}
}

