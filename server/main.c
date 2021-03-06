#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <netdb.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include "unistd.h"
#include "sender.h"
#include "fec.h"
#include "sign.h"
#include "serdes.h"
#include "hlmux.h"
#include "packetloss.h"


typedef struct TcpClient TcpClient;

#define MAX_LINE_LEN (8*1024*2)

struct TcpClient {
	int fd;
	int type;
	char buffer[MAX_LINE_LEN];
	int pos;
	int waitingForNextCycle;
	int delayAfterNextPacket;
	TcpClient *next;
};

TcpClient *clients;


int cycleLenMs=60000; //cycle defaults to 1 min
struct timeval cycleStart;

void newCycle() {
	printf("New cycle! Cycle len is %d ms\n", cycleLenMs);
	gettimeofday(&cycleStart, NULL);
}

int cycleRemainingMs() {
	struct timeval now;
	gettimeofday(&now, NULL);
	int ms=(cycleStart.tv_sec-now.tv_sec)*1000+(cycleStart.tv_usec-now.tv_usec)/1000;
	ms+=cycleLenMs;
	return ms;
}


int createSocket(int port) {
	int fd=socket(AF_INET, SOCK_STREAM, 0);
	if (fd<=0) {
		perror("creating socket");
		exit(1);
	}
	
	int optval=1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));
	
	struct sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family=AF_INET;
	addr.sin_addr.s_addr=htonl(INADDR_ANY);
	addr.sin_port=htons(port);
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr))<0) {
		perror("bind");
		exit(1);
	}

	if (listen(fd, 8)<0) {
		perror("listen");
		exit(1);
	}

	return fd;
}


static void sendResp(TcpClient *cl, int isAck) {
	char buf[2];
	buf[0]=isAck?'+':'-';
	buf[1]='\n';
	write(cl->fd, buf, 2);
}

static void sendRespNum(TcpClient *cl, int isAck, int num) {
	char buf[30];
	sprintf(buf, "%s %d\n", isAck?"+":"-", num);
	write(cl->fd, buf, strlen(buf));
}


static int hexbin(char c) {
	if (c>='0' && c<='9') return c-'0';
	if (c>='A' && c<='F') return (c-'A')+10;
	if (c>='a' && c<='f') return (c-'a')+10;
	return -1;
}

static void parseLine(char *buff, TcpClient *cl) {
	if (strlen(buff)==0) return;
	printf("Got from client: %s\n", buff);
	if (buff[0]=='t') { //set type
		int type=strtol(buff+2, NULL, 16);
		cl->type=type;
		sendResp(cl, 1);
	} else if (buff[0]=='p') { //packet
		char *n;
		int subtype=strtol(buff+2, &n, 16);
		int p=0;
		if (n!=buff+2 && *n!=0) {
			//Convert hex to bin in-place
			//Yes, this is ugly :/
			int d;
			n++; //skip space
			while ((d=hexbin(*n))!=-1) {
				if ((p&1)==0) {
					buff[p/2]=d<<4;
				} else {
					buff[p/2]|=d;
				}
				p++;
				n++;
			}
		}
		printf("Subtype %d, %d bytes\n", subtype, p/2);
		if (cl->delayAfterNextPacket) {
			serdesWaitAfterSendingNext(cl->delayAfterNextPacket);
			cl->delayAfterNextPacket=0;
		}
		hlmuxSend(cl->type, subtype, buff, p/2);
		sendResp(cl, 1);
	} else if (buff[0]=='w') {//wait for next cycle
		cl->waitingForNextCycle=1;
		//Don't respond yet; will do that when cycle ends
	} else if (buff[0]=='c') { //Get cycle length, in ms
		sendRespNum(cl, 1, cycleLenMs);
	} else if (buff[0]=='e') { //Get time to cycle end, in ms
		sendRespNum(cl, 1, cycleRemainingMs());
	} else if (buff[0]=='C') { //Set cycle len
		int i=strtol(&buff[1], NULL, 0);
		if (i<5000) {
			sendResp(cl, 0);
		} else {
			cycleLenMs=i;
			sendResp(cl, 1);
		}
	} else if (buff[0]=='W') { //Set delay after next packet
		int i=strtol(&buff[1], NULL, 0);
		if (i>1500) {
			sendResp(cl, 0);
		} else {
			cl->delayAfterNextPacket=i;
			sendResp(cl, 1);
		}
	} else {
		sendResp(cl, 0);
	}
}


static void handleClient(TcpClient *cl) {
	int foundEnter=0;
	do {
		int i;
//		printf("Buf sz %d\n", cl->pos);
		foundEnter=0;
		for (i=0; i<cl->pos; i++) {
			if (cl->buffer[i]=='\r' || cl->buffer[i]=='\n') {
				foundEnter=1;
				break;
			}
		}
		if (foundEnter) {
			cl->buffer[i]=0;	//Zero-terminate string and get rid of newline
			parseLine(cl->buffer, cl);
			memmove(cl->buffer, &cl->buffer[i+1], MAX_LINE_LEN-(i+1));
			cl->pos-=i+1;
		}
	} while (foundEnter);
}

//#define SIMULATE_PACKET_LOSS


int main(int argc, char **argv) {
	int listenFd;
	senderInit();
	if (argc==1) {
		senderAddDest("192.168.5.255");
	} else {
		senderAddDest(argv[1]);
	}
	
#ifndef SIMULATE_PACKET_LOSS
	signInit(senderSendPkt, senderGetMaxPacketLength());
#else
	packetlossInit(senderSendPkt, senderGetMaxPacketLength());
	signInit(packetlossSend, packetlossGetMaxPacketLength());
#endif
	fecInit(signSend, signGetMaxPacketLength());
	serdesInit(fecSend, fecGetMaxPacketLength());
	hlmuxInit(serdesSend, serdesGetMaxPacketLength());

	listenFd=createSocket(2017);

	newCycle();
	while(1) {
		int max;
		fd_set rfds;
		struct timeval tout;
		FD_ZERO(&rfds);
		FD_SET(listenFd, &rfds);
		max=listenFd;
		for (TcpClient *i=clients; i!=NULL; i=i->next) {
			FD_SET(i->fd, &rfds);
			if (max<i->fd) max=i->fd;
		}
		int ms=cycleRemainingMs();
		if (ms<0) ms=0;
		tout.tv_sec=ms/1000;
		tout.tv_usec=(ms%1000)*1000;
		int r=select(max+1, &rfds, NULL, NULL, &tout);
		if (r==-1) {
			perror("select");
			exit(1);
		}

		if (cycleRemainingMs()<=0) {
			//Warn all clients waiting for next cycle
			for (TcpClient *i=clients; i!=NULL; i=i->next) {
				if (i->waitingForNextCycle) {
					sendResp(i, 1);
					i->waitingForNextCycle=0;
				}
			}
			//Start next cycle
			newCycle();
			continue; //because select may not have run
		}

		if (FD_ISSET(listenFd, &rfds)) {
			//New client. Allocate struct and accept.
			TcpClient *newc=malloc(sizeof(TcpClient));
			memset(newc, 0, sizeof(TcpClient));
			newc->fd=accept(listenFd, NULL, NULL);
			newc->type=-1;
			newc->next=clients;
			newc->waitingForNextCycle=0;
			clients=newc;
			printf("Accepted client\n");
		}

		for (TcpClient *i=clients; i!=NULL; i=i->next) {
			if (FD_ISSET(i->fd, &rfds)) {
				int l=MAX_LINE_LEN-i->pos;
				if (l==0) {
					//Reached max line size. Only allow one enter to be read.
					i->pos-=1;
					l=1;
				}
				int r=read(i->fd, &i->buffer[i->pos], l);
				if (r<1) {
					//Error. Close socket, unlink client struct.
					printf("Client closed socket; cleaning up.\n");
					close(i->fd);
					if (i==clients) {
						clients=i->next;
					} else {
						//Look up struct linking to here
						TcpClient *j;
						for (j=clients; j->next!=i; j=j->next);
						j->next=i->next;
					}
					free(i);
					//Loop iterator is broken now. Bail out, select will trigger a new loop.
					break;
				} else {
					i->pos+=r;
					handleClient(i);
				}
			}
		}
	}
}
