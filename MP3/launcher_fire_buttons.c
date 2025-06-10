#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>

#define LAUNCHER_NODE           "/dev/launcher0"
#define LAUNCHER_FIRE           0x10
#define LAUNCHER_STOP           0x20
#define LAUNCHER_UP             0x02
#define LAUNCHER_DOWN           0x01
#define LAUNCHER_LEFT           0x04
#define LAUNCHER_RIGHT          0x08
#define LAUNCHER_UP_LEFT        (LAUNCHER_UP | LAUNCHER_LEFT)
#define LAUNCHER_DOWN_LEFT      (LAUNCHER_DOWN | LAUNCHER_LEFT)
#define LAUNCHER_UP_RIGHT       (LAUNCHER_UP | LAUNCHER_RIGHT)
#define LAUNCHER_DOWN_RIGHT     (LAUNCHER_DOWN | LAUNCHER_RIGHT)

#define BTN_BASE_ADDR            0x41210000
#define BTN_SIZE                 0xFA00
#define BTN_MID_MASK             0x1
#define BTN_LEFT_MASK            0x4
#define BTN_RIGHT_MASK           0x8
#define BTN_UP_MASK              0x10
#define BTN_DOWN_MASK            0x2

volatile int *btn_data;

static void launcher_cmd(int fd, int cmd) {
  int retval = 0;
  
  retval = write(fd, &cmd, 1);
  while (retval != 1) {
    if (retval < 0) {
      fprintf(stderr, "Could not send command to %s (error %d)\n", LAUNCHER_NODE, retval);
    } 

    else if (retval == 0) {
      fprintf(stdout, "Command busy, waiting...\n");
    }
  }


  if (cmd == LAUNCHER_FIRE) {
    usleep(5000000);
  }

}

int main() {
  char c;
  int fd;
  int mem_fd;
  int *btns;

  int cmd;
  char *dev = LAUNCHER_NODE;
  unsigned int duration = 500;

  if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
      perror("Couldn't open file /dev/mem");
      exit(EXIT_FAILURE);
  }

  // Map physical memory for GPIO
  btns = (int*)mmap(
      NULL,                   // Address to start mapping (let the OS choose)
      BTN_SIZE,              // Size of the mapping
      PROT_READ,              // Read-only access
      MAP_SHARED,             // Share with other processes
      mem_fd,                 // File descriptor for /dev/mem
      BTN_BASE_ADDR           // Offset to GPIO base address
  );

  if (btns == MAP_FAILED) {
      perror("mmap");
      close(mem_fd);
      exit(EXIT_FAILURE);
  }
  
  fd = open(dev, O_RDWR);
  if (fd == -1) {
    perror("Couldn't open file: %m");
    exit(1);
  }

  btn_data = (volatile int *)(btns);

  printf("Entering Button Input Loop..");
  while(1){
    
    if((*btn_data & BTN_MID_MASK) != 0){
      // Fire Pressed
      printf("mid");
      cmd = LAUNCHER_FIRE;
    }else if((*btn_data & BTN_LEFT_MASK) != 0){
      // Move Left
      printf("left");
      cmd = LAUNCHER_LEFT;
    }else if((*btn_data & BTN_RIGHT_MASK) != 0){
      // Move Right
      printf("right");
      cmd = LAUNCHER_RIGHT;
    }else if((*btn_data & BTN_DOWN_MASK) != 0){
      // Move Down
      printf("down");
      cmd = LAUNCHER_DOWN;
    }else if((*btn_data & BTN_UP_MASK) != 0){
      // Move Up
      printf("up");
      cmd = LAUNCHER_UP;
    }

    if(((*btn_data & BTN_LEFT_MASK) != 0) && ((*btn_data & BTN_RIGHT_MASK) != 0)){
      break;
    }else{
      if(cmd){
        printf("Sending command to USB Driver");
        launcher_cmd(fd, cmd);
        usleep(duration * 1000); 
        cmd = 0;
      }
    }   
  }

  printf("Exiting..");
  launcher_cmd(fd, LAUNCHER_STOP);



  close(mem_fd);
  close(fd);
  return EXIT_SUCCESS;
}

