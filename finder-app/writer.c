#include <string.h>
#include <stdlib.h>
#include <sys/syslog.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>


int main(int argc, char* argv[]){

    openlog("WriterApp", LOG_NDELAY, LOG_LOCAL0);


    // Check args first
    if (argc != 3){
        syslog(LOG_ERR, "Writer needs exactly two arguments");
        return 1;
    }

    // Store args
    const char* file_name = argv[1];
    const char* content = argv[2];

    int fd = open(file_name, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd == -1){
        syslog(LOG_ERR, "Cannot open the given file");
        return 1;
    }

    int return_val = write(fd, content, strlen(content));

    if (return_val == -1){
        syslog(LOG_ERR, "Could not complete the write operation. Ensure you have appropriate permissions");
        return 1;
    }

    syslog(LOG_INFO, "Successfully wrote content to file");


    return EXIT_SUCCESS;
}