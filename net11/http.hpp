#include <regex>
#include <map>

#include "tcp.hpp"

namespace net11 {
	class http_server : public net11::connection {
		std::function<bool(std::vector<char>&)> reqlinesink;
		std::regex rereqline;
		std::string reqline[3];

		std::function<bool(std::vector<char>&)> headsink;
		std::map<std::string,std::string> headers;
	public:
		http_server():rereqline("(\\S+)\\s+(\\S+)(?:\\s+(.+))?") {
			sink=reqlinesink=net11::make_line_sink("\r\n",4096,[this](std::string &l){
				std::smatch sma;
				if (std::regex_match(l,sma,rereqline)) {
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
			headsink=net11::make_header_sink(80,65536,
				[this](std::string &k,std::string &v){
					std::cout<<"HeadKey=["<<k<<"] HeadValue=["<<v<<"]\n";
					headers[k]=v;
					return true;
				},
				[this](const char *err) {
					std::cout<<"req:"<<reqline[0]<<" url:"<<reqline[1]<<" ver:"<<reqline[2]<<"\n";
					//conn->sink=[](std)
					return false;
				}
			);
		}
		virtual ~http_server() {
			printf("destroying httpserver\n");
		}
	};

}

