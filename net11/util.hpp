#ifndef __INCLUDED_NET11_UTIL_HPP__
#define __INCLUDED_NET11_UTIL_HPP__

#pragma once

#include <stdint.h>
#include <functional>
#include <time.h>
#include <cstring>
#include <cctype>
#include <memory>
#include <vector>
#include <string>

#ifdef _MSC_VER
#include <windows.h>
#else
#include <unistd.h>
#include <sys/time.h>
#endif

namespace net11 {
	// a sink is a data receiver
	class sink;
	// buffer is a utility class built to pass along data as a memory fifo
	class buffer;

	// a utility sink that reads a full line
	class line_parser_sink;
	// a utility sink that parses RFC 822 headers
	class header_parser_sink;

	// utility functions to create producers from a string or vector
	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T * in_data);
	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T &in_data);

	// a utility function to give up a slice of cpu time
	void yield();

	class buffer {
		bool m_isview;
		int m_cap;    // the total number of bytes in this buffer
		int m_bottom; // the bottom index, ie the first used data element
		int m_top;    // the top index, the first unused data element
		char *m_data; // the actual data
		buffer(const buffer &)=delete;
		buffer& operator=(const buffer&)=delete;
	public:
		// Construct a view of a piece of data (nothing available to fill but plenty of used to consume)
		buffer(char *data,int amount) : m_isview(true),m_cap(amount),m_bottom(0),m_top(amount),m_data(data) {
		}
		buffer(int capacity) : m_isview(false),m_cap(capacity),m_bottom(0),m_top(0),m_data(new char[capacity]) {
			//m_data=new char[capacity];
		}
		~buffer() {
			if (!m_isview)
				delete m_data;
		}
		// returns the number of bytes corrently in the buffer
		inline int usage() {
			return m_top-m_bottom;
		}
		// returns the number of bytes available to produce as a flat array
		int direct_avail() {
			return m_cap-m_top;
		}
		// returns the total number of bytes available to produce
		int total_avail() {
			return (m_cap-m_top)+(m_bottom);
		}
		// compacts the buffer to maximize the flatly available bytes
		int compact() {
			if (m_bottom==0)
				return direct_avail();
			int sz=usage();
			std::memmove(m_data,m_data+m_bottom,sz);
			m_bottom=0;
			m_top=sz;
			return direct_avail();
		}
		inline int peek() {
			if (m_bottom>=m_top)
				return -1;
			return m_data[m_bottom]&0xff;
		}
		// consumes one byte from the currently available bytes
		inline char consume() {
			if (m_bottom>=m_top)
				throw std::out_of_range("no bytes to consume in buffer");
			return m_data[m_bottom++];
		}
		// returns the pointer to a number of bytes to consume directly.
		char* to_consume() {
			return m_data+m_bottom;
		}
		// tells the buffer how many bytes was consumed
		void consumed(int amount) {
			if (usage()<amount || amount<0)
				throw std::invalid_argument("underflow");
			m_bottom+=amount;
		}
		// adds a byte to the buffer
		inline void produce(char c) {
			if (direct_avail()<1) {
				if (compact()<1) {
					throw std::out_of_range("no bytes available in buffer");
				}
			}
			m_data[m_top++]=c;
		}
		// adds as many bytes as possible from the source to this buffer
		void produce(buffer &source) {
			// how much do we want to copy if possible?
			int to_copy=source.usage();
			// now check the actual number of bytes we can copy
			if (to_copy>total_avail())
				to_copy=total_avail();
			produce(source,to_copy);
		}
		// copy the number of bytes from the source
		void produce(buffer &source,int to_copy) {
			// if we can fit it straight away copy it directly
			if (direct_avail()<to_copy) {
				// we can't copy it directly, then compact first before copy
				if (compact()<to_copy) {
					// still not enough space, fault here!
					throw std::invalid_argument("not enough space to take the copied bytes");
				}
			}
			// now copy the amount we can take
			std::memcpy(to_produce(),source.to_consume(),to_copy);
			produced(to_copy);
			source.consumed(to_copy);
		}
		// returns a the pointer to the free bytes to be written
		char* to_produce() {
			return m_data+m_top;
		}
		// tell the buffer how many bytes were actually written
		void produced(int amount) {
			if (direct_avail()<amount)
				throw std::invalid_argument("overflow");
			m_top+=amount;
		}
		// convert the buffer contents to a string
		std::string to_string() {
			std::string out;
			for (int i=m_bottom;i<m_top;i++) {
				out.push_back(m_data[i]);
			}
			return out;
		}
	};

	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T * in_data) {
		std::shared_ptr<int> off(new int);
		std::shared_ptr<T> data(in_data);
		*off=0;
		// return the actual producer function that writes out the contents on request
		return [data,off](buffer &ob){
			int dataleft=data->size()-*off; // dl is how much we have left to send
			int outleft=ob.compact();
			int to_copy=dataleft<outleft?dataleft:outleft;
			std::memcpy(ob.to_produce(),data->data()+*off,to_copy);
			ob.produced(to_copy);
			*off+=to_copy;
			return *off!=data->size();
		};
	}

	template<typename T>
	std::function<bool(buffer &)> make_data_producer(T &in_data) {
		return make_data_producer(new T(in_data));
	}

	class sink {
	public:
		// implement this function to make a working sink
		virtual bool drain(buffer &buf)=0;
	};



	class line_parser_sink : public sink {
		std::string out;  // the output string
		const char *term; // the line terminator
		int tl;           // length of the terminator string
		int szmax;        // the maximum number of bytes in a line
		std::function<bool(std::string&)> on_line;
	public:
		line_parser_sink(
			const char *in_term,
			int in_max,
			std::function<bool(std::string&)> in_on_line
		):
			term(in_term),
			szmax(in_max),
			on_line(in_on_line)
		{
			tl=strlen(term);
		}
		virtual bool drain(buffer &buf) {
			size_t sz=out.size();
			while(buf.usage()) {
				if (sz>=szmax) {
					return false;
				}
				//std::cout<<"Pre:["<<out<<"]["<<buf.to_string()<<"]\n";
				out.push_back(buf.consume());
				//std::cout<<"Post:["<<out<<"]["<<buf.to_string()<<"]\n";
				sz++;
				if (sz>tl) {
					if (!memcmp(out.data()+sz-tl,term,tl)) {
						out.resize(sz-tl);
						//std::cout<<"Line:"<<out<<":\n";
						bool rv=on_line(out);
						out.resize(0);
						return rv;
					}
				}
			}
			return true;
		}
	};

	class header_parser_sink : public sink {
		enum headerstate {
			firstlinestart=0,
			linestart,
			testemptyline,
			inkey,
			postkeyskip,
			invalue,
			postvalue
		};
		headerstate state;
		std::string k;
		std::string v;
		int count;
		int maxsz;
		int (*filter)(int c);

		std::function<bool(std::string&,std::string&)> on_header;
		std::function<bool(const char *err)> on_fin;
	public:
		header_parser_sink(
			int in_maxsz,
			int (*in_filter)(int c),
			std::function<bool(std::string&,std::string&)> in_on_header,
			std::function<bool(const char *err)> in_on_fin
		):
			state(firstlinestart),
			count(0),
			maxsz(in_maxsz),
			filter(in_filter),
			on_header(in_on_header),
			on_fin(in_on_fin)
		{}
		virtual bool drain(buffer &buf) {
			// pre-existing error condition, just return.
			if (count==-1)
				return false;
			while(buf.usage()) {
				if (count>=maxsz) {
					on_fin("Error, headers too large");
					count=-1;
					return false;
				}
				char c=buf.consume();
				count++;
				switch(state) {
				case firstlinestart :
				case linestart :
					if (c==13) {
						if (state!=firstlinestart) {
							on_header(k,v);
							k.clear();
							v.clear();
						}
						// empty line in progress
						state=testemptyline;
						continue;
					} else if (c==10) {
						on_fin("spurios LF");
						count=-1;
						return false;
					}
					if (state!=firstlinestart) {
						if (isspace(c)) {
							state=invalue;
							v.push_back(c);
							continue;
						}
						on_header(k,v);
						k.clear();
						v.clear();
					}
					if (isspace(c))
						continue;
					state=inkey;
					if (filter)
						c = filter(c);
					k.push_back(c);
					continue;
				case testemptyline :
					if (c==10) {
						// empty line encountered, we're finished with the data
						bool rv=on_fin(nullptr);
						k.clear();
						v.clear();
						state=firstlinestart;
						count=0;
						return rv;
					} else {
						on_fin("cr but no lf in empty headerline");
						count=-1;
						return false;
					}
				case inkey :
					if (c==':') {
						state=postkeyskip;
						continue;
					} else {
						if (filter)
							c=filter(c);
						k.push_back(c);
						continue;
					}
				case postkeyskip :
					if (isspace(c)) {
						continue;
					} else {
						state=invalue;
						v.push_back(c);
						continue;
					}
				case invalue :
					if (c==13) {
						state=postvalue;
						continue;
					} else {
						v.push_back(c);
						continue;
					}
				case postvalue :
					if (c==10) {
						state=linestart;
						continue;
					} else {
						on_fin("cr but no lf in headerline");
						count=-1;
						return false;
					}
				default:
					printf("headerparser unhandled state:%d\n",state);
					exit(-1);
				}
			}
			return true;
		}
	};

	void yield() {
		// TODO: IOCP/KEVENT...
	#ifdef _MSC_VER
		Sleep(1);
	#else
		usleep(10000);
	#endif
	}

	uint64_t current_time_millis() {
#ifdef _MSC_VER
		SYSTEMTIME st;
		GetSystemTime(&st);
		return st.wMilliseconds+(1000*(uint64_t)time(0));
#else
		struct timeval tv;
		gettimeofday(&tv,NULL);
		return (tv.tv_usec/1000)+(1000*(uint64_t)time(0));
#endif
	}

	class scheduler {
		struct event {
			//uint64_t next;
			uint64_t period; // for recurring events
			std::function<void()> once;
			std::function<bool()> recurring;
			event(std::function<void()> f):period(0),once(f) {}
			event(uint64_t in_period,std::function<bool()> f):period(in_period),recurring(f) {}
		};
		std::multimap<uint64_t,event> events;
	public:
		void timeout(uint64_t timeout,std::function<void()> f) {
			uint64_t event_time=current_time_millis()+timeout;
			events.emplace(event_time,event(f));
		}
		void interval(uint64_t timeout,uint64_t period,std::function<bool()> f) {
			uint64_t event_time=current_time_millis()+timeout;
			events.emplace(event_time,event(period,f));
		}
		void poll() {
			uint64_t now=current_time_millis();
			while(!events.empty()) {
				auto bi=events.begin();
				if (bi->first>now)
					break;
				if (bi->second.period) {
					if (bi->second.recurring()) {
						uint64_t next=bi->first+bi->second.period;
						events.emplace(next,event(bi->second.period,bi->second.recurring));
						bi=events.begin();
					}
				} else {
					bi->second.once();
				}
				events.erase(bi);
			}
		}
	};

	int stricmp(const std::string &l,const std::string &r) {
		size_t count=l.size()<r.size()?l.size():r.size();
		for (size_t i=0;i<count;i++) {
			char lv=std::tolower(l[i]);
			char rv=std::tolower(r[i]);
			if (lv!=rv)
				return lv-rv;
		}
		return l.size()-r.size();
	}
	bool strieq(const std::string &l,const std::string &r) {
		return 0==stricmp(l,r);
	}
	
	std::pair<std::string,std::string> split(const std::string &s,char delim) {
		std::pair<std::string,std::string> out;
		size_t dp=s.find(delim);
		// just take the find up until the match (or to the end if no match)
		out.first.append(s,0,dp);
		// create an adjusted start position for the second string based on find status and position
		size_t sp=(dp==std::string::npos)?s.size():dp+1;
		out.second.append(s,sp,s.size()-sp);
		return out;
	}
	void ltrim(std::string &v) {
		size_t ec=0;
		while(ec<v.size() && std::isspace( v[ec] ))
			ec++;
		if (ec)
			v.erase(0,ec);
	}
	void rtrim(std::string &v) {
		size_t ec=0;
		while(ec<v.size() && std::isspace( v[v.size()-ec-1] ))
			ec++;
		if (ec)
			v.erase(v.size()-ec,ec);
	}
	void trim(std::string &v) {
		rtrim(v);
		ltrim(v);
	}
	template<class I,class R,class FT>
	I filterOr(R v,FT f,I elval) {
		if (v)
			return f(v);
		return elval;
	}
}

#endif // __INCLUDED_NET11_UTIL_HPP__
