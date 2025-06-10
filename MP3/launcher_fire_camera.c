#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <math.h>

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

#define DISP_WIDTH                 1920 // 15% = 288
#define DISP_HEIGHT                1080 // 15% = 162
#define BYTES_PER_PIX              2
#define FRAME_BASE_ADDR            0x10000000
#define FRAME_SIZE                 DISP_WIDTH * DISP_HEIGHT * BYTES_PER_PIX

#define FRAME_CENTER_X             960 
#define FRAME_CENTER_Y             540 
#define FRAME_CENTER_WIN_X         30         
#define FRAME_CENTER_WIN_Y         30
#define DELAY_MIN                  50
#define DELAY_MAX                  250
#define MAX_DIST_X                 (FRAME_CENTER_X - FRAME_CENTER_WIN_X)
#define MAX_DIST_Y                 (FRAME_CENTER_Y - FRAME_CENTER_WIN_Y)

volatile int *frame_data;


typedef enum {
    LEFT,
    RIGHT,
    UP,
    DOWN
} dir;

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

void YCbCr_to_RGB(int YCbCr[3], int RGB[3]) {
    int R, G, B;

    R = YCbCr[0] + 1.402 * (YCbCr[2] - 128);
    G = YCbCr[0] - 0.344136 * (YCbCr[1] - 128) - 0.714136 * (YCbCr[2] - 128);
    B = YCbCr[0] + 1.772 * (YCbCr[1] - 128);

    // Clip the values
    R = (R < 0) ? 0 : (R > 255) ? 255 : R;
    G = (G < 0) ? 0 : (G > 255) ? 255 : G;
    B = (B < 0) ? 0 : (B > 255) ? 255 : B;

    RGB[0] = R;
    RGB[1] = G;
    RGB[2] = B;
}

int main() {
  char c;
  int fd;
  int mem_fd;
  int *frame_ptr;

  int cmd;
  char *dev = LAUNCHER_NODE;
  unsigned int duration = 500;

  printf("Starting Sentry Application\n");

  if ((mem_fd = open("/dev/mem", O_RDWR | O_SYNC)) < 0) {
      perror("Couldn't open file /dev/mem");
      exit(EXIT_FAILURE);
  }

  printf("About to use mmap()\n");
  // Map physical memory for GPIO
  frame_ptr = (int*)mmap(
      NULL,                   // Address to start mapping (let the OS choose)
      FRAME_SIZE,             // Size of the mapping
      PROT_READ,              // Read-only access
      MAP_SHARED,             // Share with other processes
      mem_fd,                 // File descriptor for /dev/mem
      FRAME_BASE_ADDR         // Offset to GPIO base address
  );

  if (frame_ptr == MAP_FAILED) {
      perror("mmap");
      close(mem_fd);
      exit(EXIT_FAILURE);
  }
  printf("Opening Launcher0 Device File\n");
  fd = open(dev, O_RDWR);
  if (fd == -1) {
    perror("Couldn't open file: %m");
    exit(1);
  }

  frame_data = (volatile int *)(frame_ptr);


  //Read Val
  //0xF0525A52;
  //[Cr][Y1][Cb][Y0]
  int pixel_data;
  int pixel1_x;
  int pixel2_x;
  int pixel1_y;
  int pixel2_y;
  int Y1;
  int Y0;
  int Cb;
  int Cr;
  int non_filtered_cnt = 0;
  int x_sum;
  int y_sum;
  int mean_x;
  int mean_y;
  dir nxt_dir = 0;
  int YCbCr[3];
  int RGB[3];
  int stop_cmd_delay;




  printf("Attempting to access frame buff data\n");
  for(int i = 0; i < 10; i++){
    pixel_data = frame_data[i];
    printf("Access #%d - %X\n", i, pixel_data);
  }


  printf("Entering Processing Loop\n");
  while(1){
    non_filtered_cnt = 0;
    x_sum = 0;
    y_sum = 0;
    // Loop over all pixels
    for (int row = 0; row < DISP_HEIGHT; row++) {
      for (int col = 0; col < DISP_WIDTH / 2; col++) {
        // Calculate the offset for the current pixel
        int offset = (row * (DISP_WIDTH / 2)) + col;
        //printf("Offset %d\n", offset);
        // Access the pixel data using frame_data[offset]
        pixel_data = frame_data[offset];
        //printf("Pixel Data %X\n", pixel_data);
        pixel1_x = (col * 2);
        pixel2_x = (col * 2) + 1;
        pixel1_y = row;
        pixel2_y = row;


        //printf("About to extract YCbCr Values");
        // Extract Y, Cb, and Cr values for each pixel
        YCbCr[2] = (pixel_data >> 24) & 0xFF;  // Cr value (shared for both pixels)
        Y1 = (pixel_data >> 16) & 0xFF;  // Y value for the first pixel
        YCbCr[1] = (pixel_data >> 8) & 0xFF;   // Cb value (shared for both pixels)
        Y0 = pixel_data & 0xFF;          // Y value for the second pixel
        
        YCbCr[0] = Y0;
        YCbCr_to_RGB(YCbCr, RGB);

        // Center Green  R - 41, G - 92, B - 39
        // Boundary Green (Light) R - 25, G - 200, B - 17
        // Boundary Green (Dark) R - 43, G - 66, B - 50


        if((RGB[0] > 25 && RGB[0] < 43) && (RGB[1] > 66 && RGB[1] < 200) && (RGB[2] > 17 && RGB[2] < 50)){
          //printf("RGB: %02X, %02X, %02X\n", RGB[0], RGB[1], RGB[2]);
          //printf("Pixel Passed\n");
          //printf("pixel1 location (%d, %d)\n", pixel1_x - FRAME_CENTER_X, pixel1_y - FRAME_CENTER_Y);
          non_filtered_cnt++;
          x_sum += pixel1_x;
          y_sum += pixel1_y;
        }
        
        YCbCr[1] = Y0;
        YCbCr_to_RGB(YCbCr, RGB);
        
        if((RGB[0] > 25 && RGB[0] < 43) && (RGB[1] > 66 && RGB[1] < 200) && (RGB[2] > 17 && RGB[2] < 50)){
          non_filtered_cnt++;
          x_sum += pixel2_x;
          y_sum += pixel2_y;
        }

      }
    }
    printf("----------------------------\n");
    printf("non_filtered_cnt : %d\n", non_filtered_cnt);
    printf("----------------------------\n");
    printf("Making Decision..\n");
    if(non_filtered_cnt > 0){
      // Find mean value
      mean_x = (int)(x_sum / non_filtered_cnt);
      mean_y = (int)(y_sum / non_filtered_cnt);

      // Mean value in relation to center of camera view
      mean_x = mean_x - FRAME_CENTER_X;
      mean_y = mean_y - FRAME_CENTER_Y;

      printf("Target : (%d, %d)\n", mean_x, mean_y);

      // Determine Launcher Direction
      if((mean_x * mean_x) > (mean_y*mean_y)){
        nxt_dir = (mean_x > 0)? RIGHT:LEFT;
        stop_cmd_delay = (int)(abs(mean_x) / MAX_DIST_X) * DELAY_MAX;
      }else{
        nxt_dir = (mean_y > 0)? DOWN:UP;
        stop_cmd_delay = (int)(abs(mean_y) / MAX_DIST_Y) * DELAY_MAX;
      }


      // Check if close enough to center, otherwise move in direction given by nxt_dir
      if((mean_x * mean_x) < (FRAME_CENTER_WIN_X * FRAME_CENTER_WIN_X) && (mean_y * mean_y) < (FRAME_CENTER_WIN_Y * FRAME_CENTER_WIN_Y)){
        // Stop Moving & Fire
        printf("Stop & Fire\n");
        launcher_cmd(fd, LAUNCHER_STOP);
        launcher_cmd(fd, LAUNCHER_UP);
        usleep((125) * 1000);
        launcher_cmd(fd, LAUNCHER_STOP);
        usleep(duration * 1000); 
        launcher_cmd(fd, LAUNCHER_FIRE);
        usleep(duration * 1000); 

      }else{
        // Move Launcher
        printf("Move Launcher %d\n", nxt_dir);
        switch (nxt_dir) {
            case LEFT:
                printf("Direction is LEFT\n");
                cmd = LAUNCHER_LEFT;
                break;
            case RIGHT:
                printf("Direction is RIGHT\n");
                cmd = LAUNCHER_RIGHT;
                break;
            case UP:
                printf("Direction is UP\n");
                cmd = LAUNCHER_UP;
                break;
            case DOWN:
                printf("Direction is DOWN\n");
                cmd = LAUNCHER_DOWN;
                break;
        }

        launcher_cmd(fd, cmd);
        usleep(((stop_cmd_delay > DELAY_MIN)? stop_cmd_delay:DELAY_MIN) * 1000);
        launcher_cmd(fd, LAUNCHER_STOP);
      }
    }else{
      launcher_cmd(fd, LAUNCHER_STOP);
    }
  }

  printf("Exiting..\n");
  //launcher_cmd(fd, cmd);
  //usleep(duration * 1000); 

  launcher_cmd(fd, LAUNCHER_STOP);



  close(mem_fd);
  close(fd);
  return EXIT_SUCCESS;
}

