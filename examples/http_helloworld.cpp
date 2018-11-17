#include <iostream>
#include <net11/http.hpp>


int main(int argc,char **argv) {
	// the tcp object manages all connections
	net11::tcp tcp;

	// start listening for http requests on port 8080
	if (net11::http::start_server(tcp,8080,
			// the routing function translates from an URL to a response
			[&](net11::http::connection &c)->net11::http::action {
				// right now we only respond with the URL text.
				return net11::http::make_text_response(200, "Hello world at "+c.url());
			}
		)
	) {
		// should not fail unless port 8080 is busy
		printf("Error listening\n");
		return -1;
	}

	// request the TCP system to do som work
	while(tcp.poll()) {
		// then sleep for a short while to make sure we don't eat up all CPU time.
		net11::yield();
	}

	return 0;
}
