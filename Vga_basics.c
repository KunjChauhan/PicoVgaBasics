
/*
* HARDWARE CONNECTIONS
* - GPIO 16 --> VGA HSYNC
* - GPIO 17 --> VGA VSYNC
* - GPIO 18 --> 330 ohm resistor --> VGA RED
* - GPIO 19 --> 330 ohm resistor --> VGA GREEN
* - GPIO 20 --> 330 ohm resistor --> VGA BLUE
* - RP2040 GND ---> VGA GND
*
* RESOURCES USED  
*  - PIO state machines 0, 1, and 2 on PIO instance 0
*  - DMA channels 0 and 1
*/
#include <stdio.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "vsync.pio.h"
#include "hsync.pio.h"
#include "rgb.pio.h"
#include "math.h"


// VGA timing constants
#define H_ACTIVE   655    // (active + frontporch - 1) - one cycle delay for mov
#define V_ACTIVE   479    // (active - 1)
#define RGB_ACTIVE 319    // (horizontal active)/2 - 1

#define TXCOUNT 153600    // Total pixels/2 (since we are using 2 pixels per byte)

unsigned char vga_data_array[TXCOUNT];
char * address_pointer = &vga_data_array[0];

#define HSYNC       16
#define VSYNC       17
#define RED_PIN     18
#define GREEN_PIN   19
#define BLUE_PIN    20

#define BLACK   0
#define RED     1
#define GREEN   2
#define YELLOW  3
#define BLUE    4
#define MAGENTA 5
#define CYAN    6
#define WHITE   7

#define TWO_PI   6.283
#define TWO_PI_F 12.566
#define WAVE_SPEED 0.05

// A function for drawing a pixel with a specified color.
// Note that because information is passed to the PIO state machines through
// a DMA channel, we only need to modify the contents of the array and the
// pixels will be automatically updated on the screen.
// so we take x, y location cordinates and also the color value , what happens is we first get the pixel value 0 ~ 307,200
// but we are using one array location for 2 pixel to reduce the array size 
// so we divide the pixel value to two  i.e if we have x=1,y=1 : pixel = 641 /2 = 320 
// so loc 320 of array will store 2 pixel value 8bit : 000101010 -> then send to fifo
void drawPixel(int x, int y, char color) {
    // Range checks
    if (x > 639) x = 639 ;
    if (x < 0) x = 0 ;
    if (y < 0) y = 0 ;
    if (y > 479) y = 479 ;

    // Which pixel is it?
    int pixel = ((640 * y) + x) ;

    // Is this pixel stored in the first 3 bits
    // of the vga data array index, or the second
    // 3 bits? Check, then mask.
    if (pixel & 1) {
        vga_data_array[pixel>>1] |= (color << 3) ;
    }
    else {
        vga_data_array[pixel>>1] |= (color) ;
    }
}

int findYcoord(float Angle, int screenX, int screenY, float offset) {
   // float sinewave = sin(Angle*0.0174 - offset)
   // return (screenHeight / 2) + (int)(sineValue * WAVE_AMPLITUDE);
    float sineValue = -sin(Angle + offset); // Add waveOffset to create movement
    return (480 / 2) + (int)(sineValue * 100);
 // return floor((screenY / 2) - (sin(Angle*0.0174 - offset)) * (screenY/4));
}

int main()
{
    stdio_init_all();
    
    PIO pio = pio0;

    uint hsync_offset = pio_add_program(pio, &hsync_program);
    uint vsync_offset = pio_add_program(pio, &vsync_program);
    uint rgb_offset = pio_add_program(pio, &rgb_program);
    // Manually select a few state machines from pio instance pio0.
    uint hsync_sm = 0;
    uint vsync_sm = 1;
    uint rgb_sm = 2;
    // Call the initialization functions that are defined within each PIO file.
    hsync_program_init(pio, hsync_sm, hsync_offset, HSYNC);
    vsync_program_init(pio, vsync_sm, vsync_offset, VSYNC);
    rgb_program_init(pio, rgb_sm, rgb_offset, RED_PIN);

    // ============================== DMA Data Channels =================================================

    // DMA channels - sends color data , 1 reconfigures and restarts 0
    int rgb_chan_0 = 0;
    int rgb_chan_1 = 1;

    // Channel Zero (sends color data to PIO VGA machine)
    dma_channel_config c0 = dma_channel_get_default_config(rgb_chan_0);     // default configs
    channel_config_set_transfer_data_size(&c0,DMA_SIZE_8);                  // 8-bit txfers
    channel_config_set_read_increment(&c0, true);                           // yes read incrementing
    channel_config_set_write_increment(&c0,false);                          // no write incrementing
    channel_config_set_dreq(&c0, DREQ_PIO0_TX2);                            // DREQ_PIO0_TX2 pacing (FIFO)
    channel_config_set_chain_to(&c0, rgb_chan_1);                           // chain to other channel

    dma_channel_configure(
        rgb_chan_0,                 // Channel to be configured
        &c0,                        // The configuration we just created
        &pio->txf[rgb_sm],          // write address (RGB PIO TX FIFO)
        &vga_data_array,            // The initial read address (pixel color array)
        TXCOUNT,                    // Number of transfers; in this case each is 1 byte.
        false                       // Don't start immediately.
    );

    // Channel One (reconfigures the first channel)
    dma_channel_config c1 = dma_channel_get_default_config(rgb_chan_1);   // default configs
    channel_config_set_transfer_data_size(&c1, DMA_SIZE_32);              // 32-bit txfers
    channel_config_set_read_increment(&c1, false);                        // no read incrementing
    channel_config_set_write_increment(&c1, false);                       // no write incrementing
    channel_config_set_chain_to(&c1, rgb_chan_0);                         // chain to other channel

    dma_channel_configure(
        rgb_chan_1,                         // Channel to be configured
        &c1,                                // The configuration we just created
        &dma_hw->ch[rgb_chan_0].read_addr,  // Write address (channel 0 read address)
        &address_pointer,                   // Read address (POINTER TO AN ADDRESS)
        1,                                  // Number of transfers, in this case each is 4 byte
        false                               // Don't start immediately.
    );

    // Initialize PIO state machine counters
    pio_sm_put_blocking(pio, hsync_sm, H_ACTIVE);
    pio_sm_put_blocking(pio, vsync_sm, V_ACTIVE);
    pio_sm_put_blocking(pio, rgb_sm, RGB_ACTIVE);

    // Start the two pio machine IN SYNC
    pio_enable_sm_mask_in_sync(pio, ((1u << hsync_sm) | (1u << vsync_sm) | (1u << rgb_sm)));

    dma_start_channel_mask((1u << rgb_chan_0)) ;

    float waveOffset = 0.0; // Offset for moving the wave
    while (true) {
        
        for (int y  = 0; y < 480 ; y++)
            {
                for (int x = 0; x < 640; x ++)
                {
                    drawPixel(x,y, 0);
                }
            }
        
        // Calculate the angle in radians for the sine function.
        //float angle ;

        for (float x=0; x < 640; x++) {    
            float angle = x*0.01 + waveOffset ;
            int y = findYcoord(angle,640,480,waveOffset);

             if (y < 0) y = 0; 
            if (y >= 480) y = 480 - 1;

            drawPixel(x, y, 2); 
            //drawPixel(x-1,y-1,0);
        }
            waveOffset += WAVE_SPEED;
            
            sleep_ms(1000);
        
    }
}
