#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <arpa/inet.h>

#define SYS_file_transfer 462  // 시스템 콜 번호, 해당 번호는 커널 설정에 따라 다를 수 있음

// 시스템 콜 호출을 위한 함수 선언
long file_transfer_syscall(const char *filename, const char *ip) {
    return syscall(SYS_file_transfer, filename, ip);
}

int main(int argc, char *argv[]) {
    // 사용법이 잘못되었을 경우 사용법을 출력하고 종료
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <file_to_send> <client_ip>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    const char *filename = argv[1]; // 전송할 파일 이름
    const char *client_ip = argv[2]; // 전송할 클라이언트의 IP 주소
    long result;

    // 파일 전송 시작 메시지 출력
    printf("Initiating file transfer for file: %s to client: %s\n", filename, client_ip);

    // 시스템 콜 호출
    result = file_transfer_syscall(filename, client_ip);
    if (result != 0) {
        // 시스템 콜이 실패했을 경우 에러 메시지 출력
        fprintf(stderr, "File transfer failed with error code %ld: %s\n", result, strerror(errno));
        exit(EXIT_FAILURE);
    }

    // 시스템 콜이 성공했을 경우 성공 메시지 출력
    printf("File transfer completed successfully.\n");
    return 0;
}