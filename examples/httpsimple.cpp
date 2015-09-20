#include <iostream>
#include <net11/http.hpp>

int main(int argc,char **argv) {
	net11::tcp l;

	// start listening for http requests
	if (l.listen(8080,
		// create new server instance once we've started listening
		net11::make_http_server([](net11::http_connection &c){
			std::cout<<c.method()<<" on url:"<<c.url()<<"\n";

			return nullptr;
		})

		// std::string[3] reqline
		// std::map<std::string,std::string> headers
		// 

		// ^- should pass a closure that handles pre-parsed
		// requests to determine routing,etc
	)) {
		printf("Error listening\n");
		return -1;
	}

	while(l.poll()) {
		net11::yield();
	}

	return 0;
}
