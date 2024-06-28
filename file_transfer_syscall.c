#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/net.h>
#include <linux/in.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <net/sock.h>
#include <linux/inet.h>


/*
이 코드는 시스템 콜 호출시 실행되는 핵심 코드로서 
다음 기능을 가집니다.
1. 쓰레드를 이용한 데이터 병렬 전송 (총 4개의 쓰래드를 통해 데아터를 전송합니다.)
2. 데이터 분할 전송 데이터를 8KB로 나누어 전송합니다.
3. 로그 출력을 통해 dmesg 메시지를 통해 얼마만큼의 데이터를 보냈는지 데이터 전송여부를 확인합니다.
*/
#define CHUNK_SIZE 8192 // 각 청크의 크기를 8KB로 설정 (데이터 청크의 크기가 8KB 정도로 설정시 최소한의 손실과 적정한 속도로 전송이 되더군오)
#define SEND_DELAY_MS 10 // 청크 전송 사이에 10ms 지연 설정 (지연을 설정한 이유는 지연이 없다면 한꺼번에 많은 데이터를 동시에 전송하게 되고 그 결과 클라이언트가 잘 못받는 오류가 생겨서 입니다.)
#define NUM_WORKERS 4 // 스레드 수 (병렬로 전송할 쓰레드의 개수를 설정합니다. 총 4개의 스레드가 병렬로 데이터를 전송합니다.)

// 청크 정보를 저장하는 구조체 
struct chunk_info {
    struct socket *sock; // 데이터를 전송할 소켓
    struct sockaddr_in addr; // 클라이언트 주소 정보
    char *data; // 전송할 데이터
    size_t size; // 데이터 크기
    int chunk_index; // 청크 인덱스 (각 데이터마다 인덱스를 할당해주었는데, 이 인덱스는 추후 클라이언트가 데이터를 조합시 쓰레드 간 중복된 데이터를 받을수도 있기에 고유의 번호를 할당했습니다.)
    struct kthread_work work; // 커널 쓰레드 작업 구조체 (커널 영역의 쓰레드는 유저영역의 쓰레드 사용법과 비슷합니다.)
};


// 데이터를 전송하기 위한 함수
static void send_chunk(struct kthread_work *work) {
    static int send_count = 0; // 전송된 청크 수 카운트 (총 얼마만큼의 데이터(청크)를 보냈는지를 체크합니다.)
    struct chunk_info *chunk = container_of(work, struct chunk_info, work); // 작업 구조체에서 chunk_info 구조체를 얻어옴
    struct msghdr msg = {0}; // 메시지 헤더 초기화
    struct kvec iov[2]; // 두 개의 iovec 구조체: 하나는 인덱스, 하나는 데이터 (총 두개의 데이터 (인덱스 정보, 데이터)를 전송하기에 입출력 벡터 구조체를 두개 설정합니다.)
    int ret;

    // iovec 구조체를 설정: 0은 인덱스, 1은 데이터
    //시작 주소와 데이터 버퍼의 길이를 기록.
    iov[0].iov_base = &chunk->chunk_index;
    iov[0].iov_len = sizeof(chunk->chunk_index);
    iov[1].iov_base = chunk->data;
    iov[1].iov_len = chunk->size;

    // 메시지 헤더를 설정
    msg.msg_name = &chunk->addr;
    msg.msg_namelen = sizeof(chunk->addr);

    // 메시지를 전송 (커널 영역에서는 다음과 같은 kernel_sendmsg함수를 통해 소켓 통신을 지원합니다.)
    ret = kernel_sendmsg(chunk->sock, &msg, iov, 2, sizeof(chunk->chunk_index) + chunk->size);
    if (ret < 0) {
        printk(KERN_ERR "Failed to send chunk: %d\n", ret); // 전송 실패 시 오류 메시지 출력
    } else {
        send_count++; // 데이터 전송이 성공시 카운트를 1만큼 증가한다.
        printk(KERN_INFO "Chunk %d sent: %d bytes, Total sent chunks: %d\n", chunk->chunk_index, ret, send_count); // 전송 성공 시 정보 로그 메시지 출력
    }

    // 청크 데이터 메모리를 해제
    kfree(chunk->data);
    kfree(chunk);
}

// 시스템 콜을 정의하고 파일 전송을 처리부분, 이 부분은 시스템 콜 구현의 핵심 부분입니다.
SYSCALL_DEFINE2(file_transfer, const char __user *, filename, const char __user *, ip) {
    struct file *file;
    char *buf;
    struct sockaddr_in addr;
    struct socket *sock;
    struct kthread_worker *workers[NUM_WORKERS]; // 앞서 지정한 쓰레드 4개를 생성하여 병렬 전송을 위한 쓰레드를 설정한다.
    loff_t offset = 0;
    int ret;
    int chunk_index = 0;
    int i;

    char k_ip[16];
    // 사용자 공간에서 커널 공간으로 IP 주소를 복사
    if (copy_from_user(k_ip, ip, sizeof(k_ip))) {
        return -EFAULT;
    }

    char k_filename[256];
    // 사용자 공간에서 커널 공간으로 파일 이름을 복사
    if (copy_from_user(k_filename, filename, sizeof(k_filename))) {
        return -EFAULT;
    }

    printk(KERN_INFO "Starting system call\n");

    // UDP 통신을 위한 소켓을 생성
    ret = sock_create_kern(&init_net, AF_INET, SOCK_DGRAM, IPPROTO_UDP, &sock);
    if (ret < 0) {
        printk(KERN_ERR "Failed to create socket: %d\n", ret);
        return ret;
    }

    // 클라이언트 주소 설정
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345); // 포트를 12345로 설정
    addr.sin_addr.s_addr = in_aton(k_ip);

    // 전송할 파일을 염
    file = filp_open(k_filename, O_RDONLY, 0); //O_RDONLY: 파일 읽기 전용으로 염
    if (IS_ERR(file)) {
        sock_release(sock);
        return PTR_ERR(file);
    }

    // 파일 청크를 읽기 위한 버퍼 할당
    buf = kmalloc(CHUNK_SIZE, GFP_KERNEL);
    if (!buf) {
        filp_close(file, NULL);
        sock_release(sock);
        return -ENOMEM;
    }

    // 쓰레드를 총 4개 생성
    for (i = 0; i < NUM_WORKERS; i++) {
        workers[i] = kthread_create_worker(0, "chunk_worker");
        if (IS_ERR(workers[i])) {
            for (int j = 0; j < i; j++) {
                kthread_destroy_worker(workers[j]);
            }
            kfree(buf);
            filp_close(file, NULL);
            sock_release(sock);
            return PTR_ERR(workers[i]);
        }
    }

    // 파일을 청크 단위로 읽어 전송 작업을 큐에 추가
    while ((ret = kernel_read(file, buf, CHUNK_SIZE, &offset)) > 0) {
        struct chunk_info *chunk = kmalloc(sizeof(struct chunk_info), GFP_KERNEL); //GFP_KERNEL은 일반적인 메모리 할당 방식을 의미
        if (!chunk) {
            // 파일의 데이터가 없는 경우 종료를 위한 작업실행
            for (i = 0; i < NUM_WORKERS; i++) {
                kthread_destroy_worker(workers[i]);
            }
            kfree(buf);
            filp_close(file, NULL);
            sock_release(sock);
            return -ENOMEM;
        }

        // 청크 정보 설정들
        chunk->sock = sock;
        chunk->addr = addr;
        chunk->data = kmemdup(buf, ret, GFP_KERNEL);
        chunk->size = ret;
        chunk->chunk_index = chunk_index++;

        // 작업을 초기화하고 큐에 추가하여 병렬 전송
        kthread_init_work(&chunk->work, send_chunk);
        kthread_queue_work(workers[chunk_index % NUM_WORKERS], &chunk->work);
        /*
        kthread_queue_work함수는 쓰레드가 할 작업을 배정하는 함수입니다.
        여기서는 각 쓰레드 4개의 업무를 할당할때 라운드 로빈 방식으로 효율적으로 할당하기 위해
        각 청크 0 1 2 3 4 에대해 쓰레드 0 1 2 3 4가 할당받을수 있게 구성했습니다.
        */
        
        msleep(SEND_DELAY_MS);  // 청크 전송 사이에 지연 추가
    }

    // 리소스를 정리하고 해제
    for (i = 0; i < NUM_WORKERS; i++) {
        kthread_flush_worker(workers[i]);
        kthread_destroy_worker(workers[i]);
    }
    kfree(buf);
    filp_close(file, NULL);
    sock_release(sock);

    printk(KERN_INFO "File transfer completed\n");
    return 0;
}
