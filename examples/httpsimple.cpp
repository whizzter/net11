#define NET11_VERBOSE

#include <iostream>
#include <net11/http.hpp>

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
					if (auto r=net11::http::make_websocket(c,65536,[](std::vector<char> &msg){
						std::cout<<"WebSockMsg["<<std::string(msg.data(),msg.size())<<"]\n";
						return true;
					})) {
						r->set_header("Sec-Websocket-Protocol","beta");
						return r;
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
		net11::yield();
	}

	return 0;
}
