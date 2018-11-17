#include <iostream>
#include <net11/tcp.hpp>

// Inefficient example of a data sink that reads in data before sending it back
class echosink : public net11::sink {

	// we need to keep a reference to a connection so we know where to send data back
	net11::tcp::connection * conn;

	// temp storage vector
	std::vector<char> tmp;
public:
	echosink(net11::tcp::connection * in_conn) : conn(in_conn) {}
	~echosink()=default;

	// the drain function is invoked on sinks as soon as data has been made available.
	bool drain(net11::buffer &buf) {
		// keep an ok flag as true as long as we haven't encountered 
		bool ok=true;

		// now go through the buf as long as we have data to read.
		while(buf.usage()) {
			// read one byte at a time
			char c=buf.consume();
			
			// put it in our temp vector
			tmp.push_back(c);
		}

		// send data back if we got any
		if (tmp.size()) {
			// let's surround our data with [] characters so we can see the packet extent.
			tmp.insert(tmp.begin(),'[');
			tmp.push_back(']');

			// sending data is done by pushing a producer that contains the data we want.
			// (A more advanced producer could be written that reads in data from a file for example)
			conn->producers.push_back(net11::make_data_producer(tmp));

			// reset our tmp buf for next time.
			tmp.clear();
		}

		// as long as there was no parse problems we want to continue receiving data.
		return ok;
	}
};

int main(int argc,char **argv) {
	// the tcp object manages all connections
	net11::tcp tcp;

	// start listening for raw requests on some port.
	if (tcp.listen(1234,
		// this lambda below is invoked each time a connection is received
		// it's job is to setup listeners and/or pass data to connecting clients.
		[](net11::tcp::connection * conn){
			// in this case we install a small echo sink that sends back slightly modified data.
			conn->current_sink=std::make_shared<echosink>(conn);
		}
	)) {
		printf("Error listening\n");
		return -1;
	}

	// request the TCP system to do some work
	while(tcp.poll()) {
		// then sleep for a short while to make sure we don't eat up all CPU time.
		net11::yield();
	}

	return 0;
}

