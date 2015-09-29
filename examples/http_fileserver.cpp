#include <string.h>
#include <net11/http.hpp>

int main(int argc,char **argv) {
	net11::tcp tcp;

	if (argc<3) {
		printf("Usage: servdir portnum rootdir\n");
		return -1;
	}

	if (tcp.listen(atoi(argv[1]),net11::http::make_server(
		[argv](net11::http::connection &c) {
			if (c.url()=="/")
				c.url()="/index.html";
			return net11::http::match_file(c,"/",argv[2]);
		}
	))) {
		printf("Could not listen on %s\n",argv[1]);
		return -2;
	}
	while(tcp.poll()) {
		net11::yield();
	}
	return 0;
}
