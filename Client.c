#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/time.h>
#include <errno.h>

/*
 이 클라이언트 코드는 서버로 부터 전송받은 데이터를 다시 쓰레드를 통해 병렬로 받아옵니다.
 이후 데이터를 조합하여 온전한 동영상 데이터를 만듭니다.
 중간과제에서 쓰레드를 통한 소켓 프로그래밍을 구현하였는데,
 클라이언트는 유저레벨의 코드이므로 동일한 쓰레드 구현 함수를 사용
*/
#define CHUNK_SIZE 8192 // 각 청크의 크기를 8KB로 설정
#define TIMEOUT_SEC 30 // 데이터 수신 대기 시간을 30초로 설정
#define NUM_THREADS 4 // 수신 스레드 수

// 전송받은 데이터의 정보를 저장하는 구조체
struct chunk {
    int index; // 청크 인덱스
    char data[CHUNK_SIZE]; // 청크 데이터
};

// 전역 변수 설정
struct sockaddr_in servaddr, cliaddr; //서버와 클라이언트의 주소 설정부분
int sockfd; //소켓 디스크럽터
FILE *outfile;
pthread_mutex_t file_lock = PTHREAD_MUTEX_INITIALIZER;  //뮤텍스 초기화 방법을 의미
int received_chunks[100000] = {0}; // 이미 수신한 청크 인덱스를 기록하는 배열 (최대 청크 수에 따라 크기 조정 가능)
int total_bytes_received = 0; // 총 수신된 바이트 수
pthread_mutex_t bytes_lock = PTHREAD_MUTEX_INITIALIZER; // 뮤텍스 초기화 방법을 의미

// 수신 스레드 함수
void* receive_chunks(void *arg) {
    struct chunk buf;
    int bytes_received;
    socklen_t len;

    // 서버로 부터 데이터를 받아오는 무한 반복문
    while (1) {
        len = sizeof(servaddr);
        bytes_received = recvfrom(sockfd, &buf, sizeof(buf), 0, (struct sockaddr *)&servaddr, &len); //받아오는 데이터를 의미합니다.
        if (bytes_received < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            } else {
                perror("recvfrom");
                break;
            }
        }

        // 수신한 청크 데이터의 크기 계산, 빼는 이유는 서버에서는 인덱스 정보도 같이 전달하는데 이에 대한 정보 4바이트를 빼야 하기 때문.
        int data_size = bytes_received - sizeof(int);
        printf("Received chunk %d of size: %d bytes\n", buf.index, data_size);

        // 중복 수신 방지를 위한 과정
        /*여기서 인덱스를 설정한 이유가 혹시나 중복된 데이터를 받았을때 처리하는 과정.
        만약 배열의 값이 0일때만 해당 인덱스의 값을 1로 바꾸고 데이터 수신을 기록합니다.
        또한 이부분은 락을 걸어놓았는데 락이란 스레드끼리의 충돌을 방지하기 위한 자물쇠 같은 것을 의미합니다.
        마치 화장실칸에 들어가서 문을 잠는 것과 동일하죠
        */
        pthread_mutex_lock(&file_lock);
        if (received_chunks[buf.index] == 0) {
            received_chunks[buf.index] =1;
            // 수신한 데이터를 파일에 기록
            if (fwrite(buf.data, 1, data_size, outfile) != data_size) {
                perror("file write failed");
                pthread_mutex_unlock(&file_lock);
                break;
            }
            pthread_mutex_unlock(&file_lock);

            // 총 수신된 바이트 수 업데이트
            pthread_mutex_lock(&bytes_lock);
            total_bytes_received += data_size;
            pthread_mutex_unlock(&bytes_lock);
        } else {
            pthread_mutex_unlock(&file_lock);
        }
    }

    return NULL;
}

int main(int argc, char *argv[]) {
    struct timeval timeout;
    pthread_t threads[NUM_THREADS];
    int i;

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <output file>\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // 데이터를 수신할 소켓 생성
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket creation failed");
        exit(EXIT_FAILURE);
    }

    /*
        소켓 수신 시간 설정
        지정된 시간 내 (30초)안에 데이터를 못받을시 에러처리 지정
    */
    timeout.tv_sec = TIMEOUT_SEC;
    timeout.tv_usec = 0;
    if (setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        perror("setsockopt failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 로컬 주소에 소켓 바인딩
    memset(&cliaddr, 0, sizeof(cliaddr));
    cliaddr.sin_family = AF_INET;
    cliaddr.sin_port = htons(12345); // 포트를 12345로 설정
    cliaddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (const struct sockaddr *)&cliaddr, sizeof(cliaddr)) < 0) {
        perror("bind failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    // 수신한 데이터를 기록할 파일 열기
    outfile = fopen(argv[1], "wb");
    if (!outfile) {
        perror("file open failed");
        close(sockfd);
        exit(EXIT_FAILURE);
    }

    printf("Waiting for file transfer...\n");

    // 수신 스레드 생성 이제 각 쓰레드는 각 쓰레드 함수를 통해 데이터를 전송받는다.
    for (i = 0; i < NUM_THREADS; i++) {
        if (pthread_create(&threads[i], NULL, receive_chunks, NULL) != 0) {
            perror("pthread_create failed");
            close(sockfd);
            fclose(outfile);
            exit(EXIT_FAILURE);
        }
    }

    // 모든 수신 스레드가 종료될 때까지 대기
    for (i = 0; i < NUM_THREADS; i++) {
        pthread_join(threads[i], NULL);
    }

    fclose(outfile);
    close(sockfd);

    printf("File received and saved to %s\n", argv[1]);
    printf("Total bytes received: %d bytes\n", total_bytes_received);
    return 0;
}
