#include<stdlib.h>
#include<stdio.h>
#include<string.h>

typedef struct request_s{
  unsigned long long int starttime;
  unsigned long long int accesstime;
  unsigned long int address;
  short parallel;
} request;


void main(int argc, char *argv[]){

  FILE *fd,*out,*reqs;
  char *string=NULL;
  char resto[2048],cache[40],command[40];
  size_t size;
  unsigned long long int time, time_ant, id;
  unsigned long int address;
  short freq=0;
  int i,j,found;

  request received[1024];
  int received_count=0;

  if (argc!=5){
    printf("Usage: %s tracefile outputfile requestsfile GHz\n",__FILE__);
    exit(-1);
  }

  fd=fopen(argv[1],"r");
  out=fopen(argv[2],"w");
  reqs=fopen(argv[3],"w");
  freq=atoi(argv[4]);

  time=0;
  time_ant=0;
  while(getline(&string,&size,fd)!=-1){
    sscanf(string,"%llu %[^\n]\n",&time,resto);
    if(time_ant==0){
      fprintf(out,"%llu\t%s\n", (unsigned long long int) 0,resto);
    }else{
      fprintf(out,"%llu\t%s\n", ((time-time_ant)/1000)*freq,resto);
    }
    time_ant=time;

    sscanf(string,"%llu %llu %lx %s %s %[^\n]\n",&time,&id,&address,cache,command,resto);

    if(strstr(cache,"mm")!=NULL){

      if(strcmp(command,"read")==0){
	sscanf(string,"%llu %llu %lx %s read request %s %[^\n]\n",&time,&id,&address,cache,command,resto);
	if(strcmp(command,"receive")==0){
	  received[received_count].starttime=time;
	  received[received_count].address=address;
	  received[received_count].parallel=0;
	  received[received_count].accesstime=0;
	  for(i=0;i<received_count;i++){
	    received[i].parallel++;
	  }
	  received_count++;
	}
	if(strcmp(command,"reply")==0){
	  i=0;
	  found=0;
	  while((!found)&&(i<received_count)){
	    found=(received[i].address==address);
	    if(!found) i++;
	  }
	  if(!found){
	    printf("ADDRESS NOT FOUND FOR REPLY\n");
	    exit(-1);
	  }else{
	    fprintf(reqs,"0x%lx startTime:%llu totalTime:%llu bankTime:%llu parallelAccesses: %d\n",address,(received[i].starttime/1000)*freq,((time-received[i].starttime)/1000)*freq,
		    ((time-received[i].accesstime)/1000)*freq,received[i].parallel);
	    for(j=i+1;j<=received_count;j++){
	      received[j-1].starttime=received[j].starttime;
	      received[j-1].address=received[j].address;
	      received[j-1].parallel=received[j].parallel;
	      received[j-1].accesstime=received[j].accesstime;
	    }
	    received_count--;
	  }
	}
      }
      if(strcmp(command,"acces")==0){
	i=0;
	found=0;
	while((!found)&&(i<received_count)){
	  found=(received[i].address==address);
	  if(!found) i++;
	}
	if(found){
	  received[i].accesstime=time;
	}
      }
    }
  }

  free(string);
  fclose(reqs);
  fclose(out);
  fclose(fd);

  exit(0);
}