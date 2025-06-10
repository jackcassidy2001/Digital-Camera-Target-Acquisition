/*****************************************************************************
 * Joseph Zambreno
 * Phillip Jones
 *
 * Department of Electrical and Computer Engineering
 * Iowa State University
 *****************************************************************************/

/*****************************************************************************
 * camera_app.c - main camera application code. The camera configures the various
 * video in and video out peripherals, and (optionally) performs some
 * image processing on data coming in from the vdma.
 *
 *
 * NOTES:
 * 02/04/14 by JAZ::Design created.
 *****************************************************************************/

#include "camera_app.h"


#define DISP_WIDTH 1920
#define DISP_HEIGHT 1080
#define IMG_BUF 30
#define NEIGHBORS 8

// Sobel kerns for edge detection
int sobel_kern_x[3][3] = {
    {-1, 0, 1},
    {-2, 0, 2},
    {-1, 0, 1}
};

int sobel_kern_y[3][3] = {
    {-1, -2, -1},
    { 0,  0,  0},
    { 1,  2,  1}
};


camera_config_t camera_config;

// Main function. Initializes the devices and configures VDMA
int main() {

	camera_config_init(&camera_config);
	fmc_imageon_enable(&camera_config);
	camera_loop(&camera_config);

	return 0;
}


// Initialize the camera configuration data structure
void camera_config_init(camera_config_t *config) {

    config->uBaseAddr_IIC_FmcIpmi =  XPAR_FMC_IPMI_ID_EEPROM_0_BASEADDR;   // Device for reading HDMI board IPMI EEPROM information
    config->uBaseAddr_IIC_FmcImageon = XPAR_FMC_IMAGEON_IIC_0_BASEADDR;    // Device for configuring the HDMI board

    // Uncomment when using VITA Camera for Video input
    config->uBaseAddr_VITA_SPI = XPAR_ONSEMI_VITA_SPI_0_S00_AXI_BASEADDR;  // Device for configuring the Camera sensor
    config->uBaseAddr_VITA_CAM = XPAR_ONSEMI_VITA_CAM_0_S00_AXI_BASEADDR;  // Device for receiving Camera sensor data


    // Uncomment when using the TPG for Video input
    //config->uBaseAddr_TPG_PatternGenerator = XPAR_V_TPG_0_S_AXI_CTRL_BASEADDR; // TPG Device

    config->uDeviceId_VTC_tpg   = XPAR_V_TC_0_DEVICE_ID;                        // Video Timer Controller (VTC) ID
    config->uDeviceId_VDMA_HdmiFrameBuffer = XPAR_AXI_VDMA_0_DEVICE_ID;         // VDMA ID
    config->uBaseAddr_MEM_HdmiFrameBuffer = XPAR_DDR_MEM_BASEADDR + 0x10000000; // VDMA base address for Frame buffers
    config->uNumFrames_HdmiFrameBuffer = XPAR_AXIVDMA_0_NUM_FSTORES;            // NUmber of VDMA Frame buffers

    return;
}


void sobel_edge_detect(unsigned char* img, char threshold) {
	unsigned char res_img[DISP_HEIGHT * DISP_WIDTH];
	int grad_x;
	int grad_y;
	int dx;
	int dy;
	unsigned char pixel;

    for (int y = 1; y < DISP_HEIGHT - 1; y++) {
        for (int x = 1; x < DISP_WIDTH - 1; x++) {
            grad_x = 0;
            grad_y = 0;
            
            // Apply Sobel kern to the input image
            for (int i = -1; i <= 1; i++) {
                for (int j = -1; j <= 1; j++) {
                    dx = x + j;
                    dy = y + i;
					pixel = img[dy * DISP_WIDTH + dx];
					grad_x += pixel * sobel_kern_x[i + 1][j + 1];
					grad_y += pixel * sobel_kern_y[i + 1][j + 1];
                }
            }

            int mag = (int)(grad_x * grad_x + grad_y * grad_y);

            
            // Thresholding: Set output pixel to white (255) if magnitude exceeds threshold, otherwise black (0)
            res_img[y * DISP_WIDTH + x] = (mag > (threshold*threshold)) ? 170: 50;
        }
    }
    // Set edge pixels to black
    for (int x = 0; x < DISP_WIDTH; x++) {
        res_img[x] = 0;  // Top row
        res_img[(DISP_HEIGHT - 1) * DISP_WIDTH + x] = 0;  // Bottom row
    }
    for (int y = 0; y < DISP_HEIGHT; y++) {
        res_img[y * DISP_WIDTH] = 0;  // Left column
        res_img[y * DISP_WIDTH + DISP_WIDTH - 1] = 0;  // Right column
    }
    // Copy transformed pixels
    for (int i = 0; i < DISP_HEIGHT * DISP_WIDTH; i++) {
        img[i] = res_img[i];
    }
}

// 0 1 2
// 3 C 4
// 5 6 7
void get_neighbors(int cur_x, int cur_y, Xuint16* neighbors, Xuint16* pS2MM_Mem){
	// Neighbor Offsets
	int x_offs[NEIGHBORS] = {-1, 0, 1, -1, 1, -1, 0, 1};
	int y_offs[NEIGHBORS] = {-1, -1, -1, 0, 0, 1, 1, 1};

	// Init flags
	for(int i = 0; i < NEIGHBORS; i ++){
		neighbors[i] = 1;
	}

	// Check for borders
	if (cur_x == 0){
		neighbors[0] = neighbors[3] = neighbors[5] = 0;
	}
	if (cur_y == 0){
		neighbors[0] = neighbors[1] = neighbors[2] = 0;
	}
	if (cur_x == DISP_WIDTH - 1){
		neighbors[2] = neighbors[4] = neighbors[7] = 0;
	}
	if (cur_y == DISP_HEIGHT - 1){
		neighbors[5] = neighbors[6] = neighbors[7] = 0;
	}

	// Store Valid Neighbor Pixels
	for(int i = 0; i < NEIGHBORS; i++){
		neighbors[i] = (neighbors[i] == 0)?  pS2MM_Mem[(cur_y) * DISP_WIDTH + cur_x] : pS2MM_Mem[(cur_y + y_offs[i]) * DISP_WIDTH + cur_x + x_offs[i]];
	}

	return;
}


Xuint16 images[IMG_BUF][DISP_HEIGHT * DISP_WIDTH];
// Main (SW) processing loop. Recommended to have an explicit exit condition
void camera_loop(camera_config_t *config) {

	Xuint32 parkptr;
	Xuint32 vdma_S2MM_DMACR, vdma_MM2S_DMACR;
	int i, j;


	xil_printf("Entering main SW processing loop\r\n");


	// Grab the DMA parkptr, and update it to ensure that when parked, the S2MM side is on frame 0, and the MM2S side on frame 1
	parkptr = XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_PARKPTR_OFFSET);
	parkptr &= ~XAXIVDMA_PARKPTR_READREF_MASK;
	parkptr &= ~XAXIVDMA_PARKPTR_WRTREF_MASK;
	parkptr |= 0x1;
	XAxiVdma_WriteReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_PARKPTR_OFFSET, parkptr);


	// Grab the DMA Control Registers, and clear circular park mode.
	vdma_MM2S_DMACR = XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_TX_OFFSET+XAXIVDMA_CR_OFFSET);
	XAxiVdma_WriteReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_TX_OFFSET+XAXIVDMA_CR_OFFSET, vdma_MM2S_DMACR & ~XAXIVDMA_CR_TAIL_EN_MASK);
	vdma_S2MM_DMACR = XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_RX_OFFSET+XAXIVDMA_CR_OFFSET);
	XAxiVdma_WriteReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_RX_OFFSET+XAXIVDMA_CR_OFFSET, vdma_S2MM_DMACR & ~XAXIVDMA_CR_TAIL_EN_MASK);


	// Pointers to the S2MM memory frame and M2SS memory frame
	volatile Xuint16 *pS2MM_Mem = (Xuint16 *)XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_S2MM_ADDR_OFFSET+XAXIVDMA_START_ADDR_OFFSET);
	volatile Xuint16 *pMM2S_Mem = (Xuint16 *)XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_MM2S_ADDR_OFFSET+XAXIVDMA_START_ADDR_OFFSET+4);


	xil_printf("Start processing 1000 frames!\r\n");
	xil_printf("pS2MM_Mem = %X\n\r", pS2MM_Mem);
	xil_printf("pMM2S_Mem = %X\n\r", pMM2S_Mem);

    int *sw_addr = (int *)XPAR_GPIO_1_BASEADDR;
    int *btn_addr = (int *)XPAR_GPIO_0_BASEADDR;
    int max_index = 0;




    unsigned char img_index = 0;

    int frame_counter = 0;
    // Part 7
    while(1){
    	// Record Mode
    	if((*sw_addr & 0x00000002) != 0){
    		xil_printf("%d\n\r", frame_counter);
			for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
				pMM2S_Mem[i] = pS2MM_Mem[i];
			}
			frame_counter++;

    	}
    	if((*sw_addr & 0x00000001) != 0){
			// Middle Button Press
			if((*btn_addr & 0x00000001) != 0){
				if(img_index < IMG_BUF){
					xil_printf("Taking a Picture, Smile ;)\n\r");
					// Capture Frame, increment index, sleep
					for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
						images[img_index][i] = 	pS2MM_Mem[i];
						pMM2S_Mem[i] = images[img_index][i];
					}
					img_index += (img_index < IMG_BUF )? 1 : 0;
					max_index = (img_index > max_index)? img_index : max_index;
					xil_printf("MAX : %d\n\r", max_index);
					sleep(2);
				}else{
					xil_printf("You have no room left in your photo gallery :(\n\r");
				}
			}else{
				for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
					pMM2S_Mem[i] = pS2MM_Mem[i];
				}
			}
    	}else{
    	// Play Mode
    		if(max_index != 0){
			// Left Button Press
			if((*btn_addr & 0x00000004) != 0){
				// Decrement img index
				img_index -= (img_index > 1)? 1 : 0;
				xil_printf("IMG INDEX : %d\n\r", img_index - 1);
			}

			// Right Button Press
			if((*btn_addr & 0x00000008) != 0){
				// Increment img index
				img_index += (img_index < max_index)? 1 : 0;
				xil_printf("IMG INDEX : %d\n\r", img_index - 1);
			}

			// Display image at current img index
			for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
				pMM2S_Mem[i] = images[img_index - 1][i];
			}

			sleep(0.5);
    		}else{
    			xil_printf("You have no captured images to view..\n\r");
				for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
					pMM2S_Mem[i] = pS2MM_Mem[i];
				}
    		}
    	}

    	// Top button exits loop
    	if((*btn_addr & 0x00000010) != 0){
    		xil_printf("Exiting.. \n\r");
    		break;
    	}

    }

	//Xuint16 clr_1 = 0;
	//Xuint16 clr_2 = 0;
	//Xuint16 cur_clr = 0;
	int is_red = 0, is_green = 0, is_blue = 0;
	int red_ch = 0, grn_ch = 0, blue_ch = 0;
	unsigned char Y = 0;
	unsigned char Y0[DISP_WIDTH*DISP_HEIGHT];
	Xuint16 Cb0[DISP_WIDTH*DISP_HEIGHT], Cr0[DISP_WIDTH*DISP_HEIGHT];
	Xuint16 Cb = 0, Cr = 0;
	int x = 0, y = 0;
	Xuint16 cur_pixel_dat = 0;
	Xuint16 neighbors[NEIGHBORS];
	unsigned char threshold = 40;
	unsigned char sobel = 0;

	// 0 1 2
	// 3 C 4
	// 5 6 7

	// R G R G
	// G B G B
	// R G R G


	// Part 5
	// Run for 1000 frames before going back to HW mode
	for (j = 1; j < 1000; j++) {
		xil_printf("Cur Frame : %d\n\r", j);
		for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
			x = i % DISP_WIDTH;
			y += (x == 0 && i != 0)? 1 : 0;
			get_neighbors(x, y, neighbors, (Xuint16 *)XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_S2MM_ADDR_OFFSET+XAXIVDMA_START_ADDR_OFFSET));
			is_red = (x % 2 == 0) && (y % 2 == 0);
			is_green = (x % 2 != y % 2);
			is_blue = (x % 2 == 1) && (y % 2 == 1);

			if(is_red){
				red_ch =  pS2MM_Mem[i];
				grn_ch = (neighbors[1] + neighbors[3] + neighbors[4]  + neighbors[6]) / 4;
				blue_ch = ( neighbors[0]  + neighbors[2]  + neighbors[5]  + neighbors[7]) / 4;
			}else if(is_green){
				grn_ch = pS2MM_Mem[i];
				if(y % 2 == 0){
					red_ch = (neighbors[3] + neighbors[4]) / 2;
					blue_ch	= (neighbors[1] + neighbors[6]) / 2;
				}else{
					red_ch = (neighbors[1] + neighbors[6]) / 2;
					blue_ch	= (neighbors[3] + neighbors[4]) / 2;
				}
			}else if(is_blue){
				red_ch = ( neighbors[0]  + neighbors[2]  + neighbors[5]  + neighbors[7]) / 4;
				grn_ch = (neighbors[1] + neighbors[3] + neighbors[4]  + neighbors[6]) / 4;
				blue_ch	= pS2MM_Mem[i];
			}

			   // Frame #1 - Red pixels
			   //for (i = 0; i < storage_size / config->uNumFrames_HdmiFrameBuffer; i += 4) {
			   //	  *pStorageMem++ = 0xF0525A52;  // Red
			   // }
			Y0[i] = (unsigned char)(0.183 * red_ch + 0.614 * grn_ch + 0.062 * blue_ch + 16);
			if(sobel == 0){
				if(x % 2 == 0){
					Cb0[i] = (Xuint16)(-0.101 * red_ch - 0.338 * grn_ch + 0.439 * blue_ch + 128);
					Cr0[i] = (Xuint16)(0.439 * red_ch - 0.399 * grn_ch - 0.040 * blue_ch + 128);
				}else{
					Cb0[i] = Cb0[i - 1];
					Cr0[i] = Cr0[i - 1];
				}
			}else{
				Cb0[i] = 128;
				Cr0[i] = 128;
			}
		/*
			if(j == 0 && i == 0){
				clr_1 = pS2MM_Mem[1920*1080-i-1];
			}


		   if(j % 2 == 0){
			   cur_clr = (pS2MM_Mem[1920*1080-i-1] ==  clr_1)? clr_2 : clr_1;
		   }else{
			   cur_clr = pS2MM_Mem[1920*1080-i-1];
		   }
		 */

		  // pMM2S_Mem[i] = cur_pixel_dat;

		}
		y = 0;
		if(sobel == 1){
			sobel_edge_detect(Y0, threshold);
		}
		for (i = 0; i < DISP_WIDTH*DISP_HEIGHT; i++) {
			if(i % 2 == 0){
				cur_pixel_dat =  (Xuint16)((Cb0[i] & 0xFF) << 8)|(Y0[i] & 0xFF);
			}else{
				cur_pixel_dat = (Xuint16)((Cr0[i - 1] & 0xFF) << 8)|(Y0[i] & 0xFF);
			}

			pMM2S_Mem[i] = cur_pixel_dat;
		}
	}


	// Grab the DMA Control Registers, and re-enable circular park mode.
	vdma_MM2S_DMACR = XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_TX_OFFSET+XAXIVDMA_CR_OFFSET);
	XAxiVdma_WriteReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_TX_OFFSET+XAXIVDMA_CR_OFFSET, vdma_MM2S_DMACR | XAXIVDMA_CR_TAIL_EN_MASK);
	vdma_S2MM_DMACR = XAxiVdma_ReadReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_RX_OFFSET+XAXIVDMA_CR_OFFSET);
	XAxiVdma_WriteReg(config->vdma_hdmi.BaseAddr, XAXIVDMA_RX_OFFSET+XAXIVDMA_CR_OFFSET, vdma_S2MM_DMACR | XAXIVDMA_CR_TAIL_EN_MASK);


	xil_printf("Main SW processing loop complete!\r\n");

	sleep(5);

	// Uncomment when using TPG for Video input
	//fmc_imageon_disable_tpg(config);

	sleep(1);


	return;
}
