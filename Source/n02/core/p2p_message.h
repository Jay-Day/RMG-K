/******************************************************************************
***N***N**Y***Y**X***X*********************************************************
***NN**N***Y*Y****X*X**********************************************************
***N*N*N****Y******X***********************************************************
***N**NN****Y*****X*X************************ Make Date  : Jul 01, 2007 *******
***N***N****Y****X***X*********************** Last Update: Jul 01, 2007 *******
******************************************************************************/
#pragma once
#include "../common/k_socket.h"
#include "p2p_instruction.h"
#include "../stats.h"

#include <deque>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#define ICACHESIZE 32

#pragma pack(push, 1)

typedef struct {
    unsigned char serial;
    unsigned char length;
} p2p_instruction_head;


#pragma pack(pop)

typedef struct {
    p2p_instruction_head head;
    char body[256];
} p2p_instruction_ptr; 

//extern int PACKETLOSSCOUNT;
//extern int PACKETMISOTDERCOUNT;

//void __cdecl kprintf(char * arg_0, ...);
//void OutputHexx(const void * inb, int len, bool usespace);

class p2p_message : public k_socket {
    unsigned char      last_sent_instruction;
    unsigned char      last_processed_instruction;
	unsigned char      last_cached_instruction;
	oslist<p2p_instruction_ptr, ICACHESIZE> out_cache;
	oslist<p2p_instruction_ptr, ICACHESIZE> in_cache;
	struct rollback_packet {
		std::vector<char> data;
		sockaddr_in addr;
	};

	std::deque<rollback_packet> rollback_cache;
	std::vector<sockaddr_in> rollback_peers;
	sockaddr_in      last_packet_addr;
	bool rollback_accept_any = false;

	bool is_peer_packet(const sockaddr_in& packet_addr) const {
		return addr.sin_addr.s_addr != 0 &&
			addr.sin_port != 0 &&
			packet_addr.sin_addr.s_addr == addr.sin_addr.s_addr &&
			packet_addr.sin_port == addr.sin_port;
	}

	bool is_rollback_peer_packet(const sockaddr_in& packet_addr) const {
		if (rollback_peers.empty())
			return is_peer_packet(packet_addr);
		for (const sockaddr_in& peer : rollback_peers) {
			if (peer.sin_addr.s_addr == packet_addr.sin_addr.s_addr &&
				peer.sin_port == packet_addr.sin_port)
				return true;
		}
		return false;
	}

	bool is_rollback_packet(const char* buf, int len) const {
		static const char prefix[] = { (char)0xff, 'R', 'M', 'G', 'K', 'R', 'B', '1' };
		return len >= (int)sizeof(prefix) && memcmp(buf, prefix, sizeof(prefix)) == 0;
	}

	void queue_rollback_packet(const char* buf, int len, const sockaddr_in& packet_addr) {
		static const int prefix_len = 8;
		if (len <= prefix_len)
			return;
		rollback_packet packet;
		packet.data.assign(buf + prefix_len, buf + len);
		packet.addr = packet_addr;
		rollback_cache.push_back(packet);
	}

	bool ingest_raw_packet(bool peer_only) {
		if (!has_data_waiting)
			return false;

		sockaddr_in packet_addr;
		char buff[2024];
		int bufflen = 2024;
		if (!k_socket::check_recv(buff, &bufflen, false, &packet_addr))
			return false;
		last_packet_addr = packet_addr;

		if (is_rollback_packet(buff, bufflen)) {
			if (rollback_accept_any || is_rollback_peer_packet(packet_addr))
				queue_rollback_packet(buff, bufflen, packet_addr);
			return true;
		}

		if (peer_only && !is_peer_packet(packet_addr))
			return true;

		unsigned char instruction_count = *buff;
		char* ptr = buff + 1;
		const char* end = buff + bufflen;
		if (instruction_count > 0 && instruction_count < 15 && ptr < end) {
			if (!peer_only &&
				addr.sin_addr.s_addr != 0 &&
				addr.sin_port != 0 &&
				(packet_addr.sin_addr.s_addr != addr.sin_addr.s_addr || packet_addr.sin_port != addr.sin_port))
				return true;
			if ((size_t)(end - ptr) < sizeof(p2p_instruction_head))
				return true;
			unsigned char latest_serial = *ptr;
			int si = in_cache.size();
			unsigned char tx = latest_serial-last_cached_instruction;
			if (tx > 0 && tx < 15) {
				if (si < 0 || si > ICACHESIZE) {
					in_cache.clear();
					si = 0;
				}
				if (si + (int)tx > ICACHESIZE) {
					in_cache.clear();
					si = 0;
				}
				in_cache.set_size(si+tx);
				for (int i = si; i < si + (int)tx; i++) {
					in_cache.items[i].head.serial = 0;
					in_cache.items[i].head.length = 0;
				}
				for (int u=0; u<instruction_count; u++) {
					if ((size_t)(end - ptr) < sizeof(p2p_instruction_head))
						break;
					unsigned char serial = ((p2p_instruction_head*)ptr)->serial;
					unsigned char length = ((p2p_instruction_head*)ptr)->length;
					ptr += sizeof(p2p_instruction_head);
					if (serial == last_cached_instruction)
						break;
					if ((size_t)(end - ptr) < (size_t)length)
						break;
					unsigned char cix = serial - last_cached_instruction;
					if (cix == 0 || cix > tx) {
						ptr += length;
						continue;
					}
					PACKETLOSSCOUNT += cix - 1;
					int ind = si + (cix) - 1;
					if (ind < 0 || ind >= ICACHESIZE) {
						ptr += length;
						continue;
					}
					in_cache.items[ind].head.serial = serial;
					in_cache.items[ind].head.length = length;
					memcpy(in_cache.items[ind].body, ptr, length);
					ptr += length;
				}
				last_cached_instruction = latest_serial;
			} else if (tx==0) SOCK_RECV_RETR++; else PACKETMISOTDERCOUNT++;
		} else {
			if (instruction_count == 0 && bufflen > 5) {
				const int payload_len = bufflen - 1;
				if (payload_len > 0) {
					const size_t alloc_size = 2 + sizeof(sockaddr_in) + (size_t)payload_len;
					char* inb = (char*)malloc(alloc_size);
					if (inb) {
						*((short*)inb) = (short)payload_len;
						*((struct sockaddr_in*)(inb + 2)) = packet_addr;
						memcpy(inb + 2 + sizeof(sockaddr_in), buff + 1, payload_len);
						ssrv_cache.add(inb);
					}
				}
			}
		}
		return true;
	}
public:
	odlist<char*, 1>	ssrv_cache;
	int                 default_ipm;
	int                 dsc;
    p2p_message(){
        k_socket();
		last_sent_instruction = 0;
        last_processed_instruction = 0;
		last_cached_instruction = -1;
        default_ipm = 3;
		dsc = 0;
		memset(&last_packet_addr, 0, sizeof(last_packet_addr));
    }

    void send_instruction(p2p_instruction * arg_0){
        p2p_message::send((char*)arg_0, arg_0->size());
		////kprintf("Sent:");
		//arg_0->to_string();
    }

	bool send_instruction_to_addr(p2p_instruction *instruction, sockaddr_in *target, unsigned char *serial){
		if (instruction == NULL || target == NULL || serial == NULL)
			return false;
		char body[256];
		const int body_len = instruction->write_to_message(body);
		if (body_len < 0 || body_len > 256)
			return false;
		char buf[1 + sizeof(p2p_instruction_head) + 256];
		buf[0] = 1;
		p2p_instruction_head head;
		head.serial = (*serial)++;
		head.length = (unsigned char)body_len;
		memcpy(buf + 1, &head, sizeof(head));
		memcpy(buf + 1 + sizeof(head), body, body_len);
		sockaddr_in saved = addr;
		set_addr(target);
		const bool sent = k_socket::send(buf, 1 + (int)sizeof(head) + body_len);
		set_addr(&saved);
		return sent;
	}

	void send_tinst(int type, int flags){
		p2p_instruction kx(type, flags);
        p2p_message::send((char*)&kx, kx.size());
		////kprintf("Sent:");
		//kx.to_string();
    }

		void send_ssrv(char * bf, int bflen, sockaddr_in * arg_addr){
			sockaddr_in sad = addr;
			set_addr(arg_addr);
			char bufferr[1000];
			if (bflen < 0 || bflen > (int)sizeof(bufferr) - 1) {
				set_addr(&sad);
				return;
			}
			bufferr[0] = 0;
			memcpy(&bufferr[1], bf, bflen);
			k_socket::send(bufferr, bflen+1);
			set_addr(&sad);
	    }

		void send_ssrv(char * bf, int bflen, char * host, int port){
			sockaddr_in sad = addr;
			set_address(host, port);
			char bufferr[1000];
			if (bflen < 0 || bflen > (int)sizeof(bufferr) - 1) {
				set_addr(&sad);
				return;
			}
			bufferr[0] = 0;
			memcpy(&bufferr[1], bf, bflen);
			k_socket::send(bufferr, bflen+1);
			set_addr(&sad);
	    }

	bool has_ssrv(){
		return ssrv_cache.length > 0;
	}

	int receive_ssrv(char * buf, sockaddr_in * addrr){
		if (ssrv_cache.length > 0) {
			char * bbb = ssrv_cache[0];
			int len = *((short*)bbb);
			memcpy(buf, bbb+2+sizeof(sockaddr_in), len);
			memcpy(addrr, bbb+2, sizeof(sockaddr_in));
			free(bbb);
			ssrv_cache.removei(0);
			return len;
		}
		return 0;
	}

    bool send(char * buf, int len){
        unsigned char vx = last_sent_instruction++;
		p2p_instruction* ibuf = (p2p_instruction*)buf;

        if (out_cache.size() == ICACHESIZE-1) {
			out_cache.removei(0);
        }

		p2p_instruction_ptr kip;
		kip.head.serial = vx;
		kip.head.length = ibuf->write_to_message(kip.body);
		out_cache.add(kip);

        send_message(default_ipm);
        return true;
    }

	    bool send_message(int limit){
	        char buf[0x1000];
	        int len = 1;
	        char * buff = buf + 1;
	        char max_t = std::min((int)out_cache.size(), limit);
			char sent_t = 0;
			////kprintf(__FILE__ "%i", out_cache.size());
	        if(max_t > 0) {
	            for (int i = 0; i < max_t; i++) {
	                int cache_index = out_cache.size() - i -1;
	                *(p2p_instruction_head*)buff = out_cache[cache_index].head;
	                buff += sizeof(p2p_instruction_head);
	                int l;
					l = out_cache[cache_index].head.length;
					if ((size_t)len + sizeof(p2p_instruction_head) + (size_t)l > sizeof(buf))
						break;
	                memcpy(buff, out_cache[cache_index].body, l);
	                buff += l;
	                len += l + sizeof(p2p_instruction_head);
					sent_t++;
	            }
	        }
			buf[0] = sent_t;
			if (dsc>0)
				dsc--;
			else
				k_socket::send(buf, len);
        return true;
    }

    bool receive_instruction(p2p_instruction * arg_0, bool leave_in_queue, sockaddr_in* arg_8) {
        char var_8000[1024];
        int var_8004 = 1024;
        if (check_recv(var_8000, &var_8004, leave_in_queue, arg_8)) {
			////kprintf("Received:");
            arg_0->read_from_message(var_8000, var_8004);
			//arg_0->to_string();
            return true;
        }
		return false;
    }

	bool receive_raw_instruction(p2p_instruction *instruction, sockaddr_in *source) {
		if (!has_data_waiting || instruction == NULL)
			return false;

		sockaddr_in packet_addr;
		char packet[2024];
		int packet_len = 2024;
		if (!k_socket::check_recv(packet, &packet_len, false, &packet_addr))
			return false;
		last_packet_addr = packet_addr;

		if (is_rollback_packet(packet, packet_len)) {
			if (is_rollback_peer_packet(packet_addr))
				queue_rollback_packet(packet, packet_len, packet_addr);
			return false;
		}

		if (packet_len <= 1)
			return false;
		const unsigned char instruction_count = (unsigned char)packet[0];
		if (instruction_count == 0 || instruction_count >= 15)
			return false;
		if ((size_t)(packet_len - 1) < sizeof(p2p_instruction_head))
			return false;

		char *ptr = packet + 1;
		p2p_instruction_head *head = (p2p_instruction_head*)ptr;
		ptr += sizeof(p2p_instruction_head);
		if ((size_t)(packet + packet_len - ptr) < head->length)
			return false;
		instruction->read_from_message(ptr, head->length);
		if (source != NULL)
			*source = packet_addr;
		return true;
	}

	bool receive_instructionx(p2p_instruction * arg_0) {
        char var_8000[1024];
        int var_8004 = 1024;
        if (check_recvx(var_8000, &var_8004)) {
			////kprintf("Received:");
            arg_0->read_from_message(var_8000, var_8004);
			//arg_0->to_string();
            return true;
        }
		return false;
    }

		bool check_recvx (char* buf, int * len){
	        if (has_data_waiting) {
				ingest_raw_packet(true);
	        }
	        if (in_cache.size() > 0) {
				if (*len <= 0)
					return false;
				if ((int)in_cache[0].head.length > *len)
					return false;
				*len = in_cache[0].head.length;
				memcpy(buf, in_cache[0].body, *len);
				last_processed_instruction = in_cache[0].head.serial;
				in_cache.removei(0);
			return true;
        }
        return false;
    }

	    bool check_recv (char* buf, int * len, bool leave_in_queue, sockaddr_in* addrp){
	        if (has_data_waiting)
				ingest_raw_packet(false);
	        if (in_cache.size() > 0) {
				if (*len <= 0)
					return false;
				if ((int)in_cache[0].head.length > *len)
					return false;
				*len = in_cache[0].head.length;
				memcpy(buf, in_cache[0].body, *len);
				if (addrp != NULL)
					*addrp = last_packet_addr;
				if(!leave_in_queue) {
					last_processed_instruction = in_cache[0].head.serial;
					in_cache.removei(0);
			}
			return true;
        }
        return false;
    }

    bool has_data(){
        if (in_cache.length == 0)
            return has_data_waiting;
        else
            return true;
    }

    void resend_message(int limit){
        send_message(limit);
    }

	bool send_rollback_packet(const char* data, int len) {
		return send_rollback_packet_to_addr(&addr, data, len);
	}

	bool send_rollback_packet_to_endpoint(const char* endpoint, const char* data, int len) {
		if (endpoint == NULL)
			return false;
		std::string value(endpoint);
		const size_t separator = value.rfind(':');
		if (separator == std::string::npos)
			return false;
		std::string host = value.substr(0, separator);
		int port = atoi(value.substr(separator + 1).c_str());
		if (host.empty() || port <= 0 || port > 65535)
			return false;

		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_port = htons((u_short)port);
		target.sin_addr.s_addr = inet_addr(host.c_str());
		if (target.sin_addr.s_addr == INADDR_NONE) {
			hostent* resolved = gethostbyname(host.c_str());
			if (resolved == NULL)
				return false;
			target.sin_addr = *(struct in_addr*)(resolved->h_addr_list[0]);
		}
		return send_rollback_packet_to_addr(&target, data, len);
	}

	bool send_rollback_packet_to_addr(const sockaddr_in* target, const char* data, int len) {
		static const char prefix[] = { (char)0xff, 'R', 'M', 'G', 'K', 'R', 'B', '1' };
		if (target == NULL || data == NULL || len <= 0 || len > 2000)
			return false;
		char buf[2024];
		if ((int)sizeof(prefix) + len > (int)sizeof(buf))
			return false;
		memcpy(buf, prefix, sizeof(prefix));
		memcpy(buf + sizeof(prefix), data, len);
		sockaddr_in saved = addr;
		set_addr((sockaddr_in*)target);
		const bool sent = k_socket::send(buf, (int)sizeof(prefix) + len);
		set_addr(&saved);
		return sent;
	}

	bool poll_raw_socket() {
		return ingest_raw_packet(false);
	}

	bool receive_rollback_packet(char* data, int* len, sockaddr_in* addrp) {
		if (data == NULL || len == NULL || *len <= 0 || rollback_cache.empty())
			return false;
		const rollback_packet& packet = rollback_cache.front();
		if ((int)packet.data.size() > *len)
			return false;
		*len = (int)packet.data.size();
		memcpy(data, packet.data.data(), packet.data.size());
		if (addrp != NULL)
			*addrp = packet.addr;
		rollback_cache.pop_front();
		return true;
	}

	void clear_rollback_packets() {
		rollback_cache.clear();
	}

	void set_rollback_peers(const std::vector<std::string>& endpoints) {
		rollback_peers.clear();
		for (const std::string& endpoint : endpoints) {
			const size_t separator = endpoint.rfind(':');
			if (separator == std::string::npos)
				continue;
			std::string host = endpoint.substr(0, separator);
			int port = atoi(endpoint.substr(separator + 1).c_str());
			if (host.empty() || port <= 0 || port > 65535)
				continue;
			sockaddr_in peer = {};
			peer.sin_family = AF_INET;
			peer.sin_port = htons((u_short)port);
			peer.sin_addr.s_addr = inet_addr(host.c_str());
			if (peer.sin_addr.s_addr == INADDR_NONE) {
				hostent* resolved = gethostbyname(host.c_str());
				if (resolved == NULL)
					continue;
				peer.sin_addr = *(struct in_addr*)(resolved->h_addr_list[0]);
			}
			rollback_peers.push_back(peer);
		}
	}

	void set_rollback_accept_any(bool accept_any) {
		rollback_accept_any = accept_any;
	}
};
