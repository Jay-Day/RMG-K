/******************************************************************************
***N***N**Y***Y**X***X*********************************************************
***NN**N***Y*Y****X*X**********************************************************
***N*N*N****Y******X***********************************************************
***N**NN****Y*****X*X************************ Make Date  : Jul 01, 2007 *******
***N***N****Y****X***X*********************** Last Update: Jul 01, 2007 *******
******************************************************************************/
#include "k_socket.h"
#include "../stats.h"

void __cdecl kprintf(char * arg_0, ...);

slist<k_socket*, FD_SETSIZE> k_socket::list;
SOCKET k_socket::ndfs = 0;
fd_set k_socket::sockets;



k_socket::k_socket(){
	sock = 0;
	list.add(this);
	has_data_waiting = false;
	memset(&addr, 0, sizeof(addr));
	if(ndfs == 0) {
		FD_ZERO(&sockets);
	}
	port = 0;
}

k_socket::~k_socket(){
	//kprintf(__FILE__ ":%i", __LINE__);
	close();
	//kprintf(__FILE__ ":%i", __LINE__);
}

int k_socket::clone(k_socket * remote){
	port = remote->port;
	sock = remote->sock;
	return 0;
}

void k_socket::close(){
	//kprintf(__FILE__ ":%i", __LINE__);
	if(sock != 0) {
		shutdown(sock, 2);
		closesocket(sock);
		FD_CLR(sock, &sockets);
		if(sock == ndfs) {
			ndfs = 0;
			SOCKET temp = 0;
			k_socket * ks;
			for (int i = 0; i < list.size(); i++) {
				if ((ks = list[i]) != this && ks->sock > ndfs){
					ndfs = ks->sock;
				}
			}
		}
	}
	//kprintf(__FILE__ ":%i", __LINE__);
	list.remove(this);
	//kprintf(__FILE__ ":%i", __LINE__);
}

bool k_socket::initialize(int param_port, int minbuffersize){
	port = param_port;
	sockaddr_in tempaddr;
	memset(&tempaddr, 0, sizeof(tempaddr));
	tempaddr.sin_family = AF_INET;
	tempaddr.sin_port = htons(param_port);
	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
	if (sock != SOCKET_ERROR) {
		tempaddr.sin_addr.s_addr = htonl(0);
		if (bind(sock, (sockaddr*)&tempaddr, sizeof(tempaddr))==0) {
			unsigned long ul = 1;
			ioctlsocket (sock, FIONBIO, &ul);
			FD_SET(sock, &sockets);
			if (sock > ndfs)
				ndfs = sock;
			if (port == 0) {
				//kprintf("port is 0");
				socklen_t addrlen = sizeof(tempaddr);
				getsockname(sock, (sockaddr*)&tempaddr, &addrlen);
				port = ntohs(tempaddr.sin_port);
				//kprintf("port is %i", port);
			}
			if (minbuffersize > 0) {
				socklen_t lenn = sizeof(int);
				int val;
				getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&val, &lenn);
				if (val < minbuffersize) {
					socklen_t lenn2 = sizeof(int);
					int val2 = minbuffersize;
					setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&val2, lenn2);
				}
			}
			//getsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&val, &lenn);
			//kprintf("SO_RCVBUF: %ubytes", val);
			return true;
		} else {
			return false;
		}
	} else {
		//printf("uh oh. socket() returned -1!\n");
		return 1;
	}
}

bool k_socket::set_address(const char * cp, const u_short hostshort){
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(hostshort);
	if (addr.sin_port != 0) {
		addr.sin_addr.s_addr = inet_addr(cp);
		if (addr.sin_addr.s_addr == -1) {
			hostent * h = gethostbyname(cp);
			if (h!=0) {
				addr.sin_addr = *(struct in_addr*)(h->h_addr_list[0]);
				return true;
			}
			return false;
		}
		return true;
	}
	return false;
}

bool k_socket::set_addr(sockaddr_in * arg_addr) {
	memcpy(&addr, arg_addr, sizeof(addr));
	return true;
}
bool k_socket::set_aport(int port){
	return ((addr.sin_port = htons(port)) != 0);
}
bool k_socket::send(char * buf, int len){
/*
	int diffserv = 0;
	int lenn = 4;
	setsockopt(sock, IPPROTO_IP, IP_TOS, (char*)&diffserv, lenn);
*/
	SOCK_SEND_PACKETS++;
	SOCK_SEND_BYTES += len;
	return (sendto(sock, buf, len, 0, (sockaddr*)&addr, 16) != SOCKET_ERROR);
}
bool k_socket::check_recv (char* buf, int * len, bool leave_in_queue, sockaddr_in* addrp)  {
	struct sockaddr saa;
	socklen_t V4 = sizeof(saa);
	has_data_waiting = 0;
	int  lenn = 0;
	if ((lenn = recvfrom(sock, buf, *len, leave_in_queue? MSG_PEEK:0, &saa, &V4)) > 0) {
		*len = lenn;
		if(lenn != 0) {
			memcpy(addrp, &saa, sizeof(saa));
			SOCK_RECV_PACKETS++;
			SOCK_RECV_BYTES += lenn;
		}
		return true;
	}
	return false;
}
bool k_socket::has_data(){
	return has_data_waiting;
}
int k_socket::get_port(){
	return port;
}

char* k_socket::to_string(char *buf){
	sprintf(buf, "k_socket {\n\tsock: %llu;\n\tport: %u;\n\thas_data: %i;\n};",
		(unsigned long long)sock, (unsigned int)port, (int)has_data_waiting);
	return buf;
}

bool k_socket::check_sockets(int secs, int ms){
	
	timeval tv;
	tv.tv_sec = secs;
	tv.tv_usec = ms * 1000;

	fd_set temp;
	memcpy(&temp, &sockets, sizeof(fd_set));

	if(select((int)(ndfs + 1), &temp, 0, 0, &tv) != 0) {
		if(list.size() > 0) {
			for (int i = 0; i < list.size(); i++){
				k_socket * k = list[i];
				if (FD_ISSET(k->sock, &temp)!=0){
					k->has_data_waiting = true;
				}
			}
		}
		return true;
	}
	return false;
}

bool k_socket::Initialize(){
#ifdef _WIN32
	WSAData ws;
	return (WSAStartup(0x0202, &ws)==0);
#else
	return true;
#endif
}
void k_socket::Cleanup(){
#ifdef _WIN32
	WSACleanup();
#endif
}
