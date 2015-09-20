#include <iostream>
#include <net11/http.hpp>

int main(int argc,char **argv) {
	net11::tcp l;

	printf("Start!\n");

	if (l.listen(8080,[](){
		// create new server instance
		return new net11::http_server;

		// ^- should pass a closure that handles pre-parsed
		// requests to determine routing,etc
	})) {
		printf("Error listening\n");
		return -1;
	}

	while(l.poll()) {
		net11::yield();
	}

	return 0;
}
