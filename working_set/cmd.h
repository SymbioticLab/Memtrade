#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/user.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <string.h>

FILE* _pipe;
char* DSTAT_CMD = "dstat -Tmsg --top-mem";
const char s[4] = "| \t";
int d;

int exec_cmd( char* cmd ) {
	_pipe = popen(cmd, "r");
	if(!_pipe){
		printf("Failed to execute the command\n");
		return 0;
	}
	d = fileno(_pipe);
	fcntl(d, F_SETFL, O_NONBLOCK);
	return 1;
}

void tokenize_buffer ( char* buffer ) {
	char *token = strtok(buffer, s);
	while( token != NULL ) {
		if(!(token[0] == ' ' || token[0] == '\t' || token[0] == '\n'))
			printf( "%s ", token );
		token = strtok(NULL, s);
	}
}

void close_pipe(){
	pclose(_pipe);
}

int get_pipe_data( char* buffer, int buffer_size, int do_print ) {
	int r = read(d, buffer, buffer_size);
	if (r == -1 && errno == EAGAIN) {
		return 0;
	}
	else if (r > 0) {
		if(do_print == 1)
			printf("%s", buffer);
		return 1;
	}
	else {
		close_pipe();
		return 0;
	}
}

int run_dstat() {
    return exec_cmd(DSTAT_CMD);
}