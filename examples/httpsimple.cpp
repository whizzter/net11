#include <iostream>
#include <net11/http.hpp>

int main(int argc,char **argv) {
	net11::tcp l;

	// start listening for http requests
	if (l.listen(8080,
		// creates a new server instance once we've started listening
		net11::make_http_server(
			// the routing function
			[](net11::http_connection &c){
				std::cout<<c.method()<<" on url:"<<c.url()<<"\n";

				// simple function return
				if (c.url()=="/hello") {
					return net11::make_text_response(200,{},"Hello world!");
				}

				if (c.url()=="/") {
					c.url()="/files/index.html";
				}

				if (0==c.url().find("/files/")) {
					return net11::make_file_response("public_html/",c.url().substr(7));
				}

				// return null for a 404 response
				return (net11::http_response*)nullptr;
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
