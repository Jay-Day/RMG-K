/******************************************************************************
***N***N**Y***Y**X***X*********************************************************
***NN**N***Y*Y****X*X**********************************************************
***N*N*N****Y******X***********************************************************
***N**NN****Y*****X*X************************ Make Date  : Jul 01, 2007 *******
***N***N****Y****X***X*********************** Last Update: Jul 01, 2007 *******
******************************************************************************/
#pragma once
#include <string.h>
#include <memory.h>
#include <algorithm>

#define LOGN	0
#define PING	1
#define PREADY	2
#define TSYNC	3
#define TTIME	4
#define LOAD	5
#define START	6
#define DATA	7
#define DROP	8
#define CHAT	9
#define EXIT	10

///////////////////////////////////////////////////////////////////////////////

//////0/////1//////2//////3//////4//////5/////6//////7/////8/////9/////a/////*/
//0x00

#define LOGN_REQ 0
#define LOGN_RPOS 1
#define LOGN_RNEG 2
#define LOGN_CFM 3

#define PING_PING 0
#define PING_ECHO 1
#define PING_ROOM_START 2
#define PING_ROOM_PROBE 3
#define PING_ROOM_PROBE_ECHO 4
#define PING_ROOM_REPORT 5
#define PING_ROOM_DATA_REPORT 6

#define PREADY_READY 0
#define PREADY_NREADY 1

#define LOAD_LOAD 0
#define LOAD_LOADED 1
#define LOAD_LOADEDACK 2

#define TTIME_TTIME 0

#define START_START 0

#define DROP_DROP 0

#define TSYNC_FORCE 1
#define TSYNC_CHECK 2
#define TSYNC_ADJUST 4

#define CHAT_32 0
#define CHAT_64 1
#define CHAT_128 2

#ifndef MSTR
#define MSTR(X) #X
#endif

#pragma pack(push, 1)

typedef struct {
	unsigned type: 4;
	unsigned flags: 4;
	unsigned char body[256];
}p2p_instruction_st;

#pragma pack(pop)


#ifdef _MSC_VER
#pragma intrinsic(memcmp, memcpy, memset, strcat, strcmp, strcpy, strlen)
#endif

class p2p_instruction {
public:
	p2p_instruction_st inst;

    unsigned char pos;
	unsigned char len;

	void to_string ();
	void to_string (char*);

    p2p_instruction();
	p2p_instruction(int,int);
	
    void clone(p2p_instruction * arg_0);
	unsigned char size();

	void store_bytes(const void * arg_0, const int arg_4);
    void load_bytes(void*, const int arg_4);

    void store_string(const char * arg_0);
    void load_string(char * arg_0);
	void store_sstring(const char * arg_0);
    void load_sstring(char * arg_0);
	void store_mstring(const char * arg_0);
    void load_mstring(char * arg_0);
		void store_vstring(const char * arg_0);
		void load_vstring(char * arg_0, unsigned int arg_4);

	//void store_xstring(const char * arg_0);
    //void load_xstring(char * arg_0);

    void store_int(const int x);
	void store_uint(const unsigned int x);
	int load_int();
	unsigned int load_uint();

    void store_short(const short x);
	void store_ushort(const unsigned short x);
	short load_short();
	unsigned short load_ushort();

    void store_char(const char x);
	void store_uchar(const unsigned char x);
	char load_char();
	unsigned char load_uchar();

    int write_to_message(char * arg_0);
    void read_from_message(char * p_buffer, int p_buffer_len);
};
