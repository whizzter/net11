// Uncomment the line below to get verbose header information
//#define NET11_VERBOSE

#include <iostream>
#include <net11/http.hpp>

static net11::scheduler sched;

int main(int argc,char **argv) {
	net11::tcp l;

	// start listening for http requests
	if (l.listen(8080,
		// creates a new server instance once we've started listening
		net11::http::make_server(
			// the routing function
			[](net11::http::connection &c){
#ifdef NET11_VERBOSE
				std::cout<<c.method()<<" on url:"<<c.url()<<"\n";
#endif

				// simple function returning data on a url
				if (c.url()=="/hello") {
					return net11::http::make_text_response(200,"Hello world!");
				}

				// for the correct url, try making a websocket echo service.
				if (c.url()=="/sockettest")
					if (auto r=net11::http::make_websocket(c,65536,[](net11::http::websocket &ws,std::vector<char> &msg){
#ifdef NET11_VERBOSE
						std::cout<<"WebSockMsg["<<std::string(msg.data(),msg.size())<<"]\n";
#endif

						// reply to the socket immediately
						std::string reply1="Immediate echo:"+std::string(msg.data(),msg.size());
						ws.send(reply1);

						// wait a second before sending the second reply
						// this method together with weak points can be used
						// for handling websocket updates from other systems
						auto sws=ws.shared_from_this();
						std::string reply2="Delayed echo:"+std::string(msg.data(),msg.size());
						sched.timeout(1000,[sws,reply2](){
							sws->send(reply2);
						});

						return true;
					})) {
						r->set_header("Sec-Websocket-Protocol","beta");
						return r;
					}

				if (c.url()=="/echo") {
					if (auto r=net11::http::make_websocket(c,130000,[](net11::http::websocket &ws,std::vector<char> &msg) {
						ws.send(msg);
						return true;
					})) {
						return r;
					}
				}

				// change the / url to the indexpage
				if (c.url()=="/") {
					c.url()="/index.html";
				}

				// if no other urls has hit yet try running a file matching service.
				if (auto r=net11::http::match_file_response(c,"/","public_html/")) {
					return r;
				}

				// return null for a 404 response
				return (net11::http::response*)nullptr;
			}
		)
	)) {
		printf("Error listening\n");
		return -1;
	}

	while(l.poll()) {
		sched.poll();
		net11::yield();
	}

	return 0;
}
