// -*- mode: c++; c-basic-offset: 4; indent-tabs-mode: nil; -*-
// SPI Pixels - Control SPI LED strips (spixels)
// Copyright (C) 2016 Henner Zeller <h.zeller@acm.org>
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU Lesser General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#include "multi-spi.h"

#include "ft-gpio.h"
#include "rpi-dma.h"

#include <assert.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// mmap-bcm-register
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>


// ---- GPIO specific defines
#define GPIO_REGISTER_BASE 0x200000
#define GPIO_SET_OFFSET 0x1C
#define GPIO_CLR_OFFSET 0x28
#define PHYSICAL_GPIO_BUS (0x7E000000 + GPIO_REGISTER_BASE)

// ---- DMA specific defines
#define DMA_CHANNEL       5   // That usually is free.
#define DMA_BASE          0x007000

namespace spixels {
namespace {
class DMAMultiSPI : public MultiSPI {
public:
    explicit DMAMultiSPI(int clock_gpio);
    virtual ~DMAMultiSPI();

    virtual bool RegisterDataGPIO(int gpio, size_t serial_byte_size);
    virtual void SetBufferedByte(int data_gpio, size_t pos, uint8_t data);
    virtual void SendBuffers();

private:
    void FinishRegistration();

    struct GPIOData;
    ft::GPIO gpio_;
    const int clock_gpio_;
    size_t serial_byte_size_;   // Number of serial bytes to send.

    struct UncachedMemBlock alloced_;
    GPIOData *gpio_dma_;
    struct dma_cb* start_block_;
    struct dma_channel_header* dma_channel_;

    GPIOData *gpio_shadow_;
    size_t gpio_buffer_size_;  // Buffer-size for GPIO operations needed.
};
}  // end anonymous namespace

struct DMAMultiSPI::GPIOData {
    uint32_t set;
    uint32_t ignored_upper_set_bits; // bits 33..54 of GPIO. Not needed.
    uint32_t reserved_area;          // gap between GPIO registers.
    uint32_t clr;
};

DMAMultiSPI::DMAMultiSPI(int clock_gpio)
    : clock_gpio_(clock_gpio), serial_byte_size_(0),
      gpio_dma_(NULL), gpio_shadow_(NULL) {
    alloced_.mem = NULL;
    bool success = gpio_.Init();
    assert(success);  // gpio couldn't be initialized
    success = gpio_.AddOutput(clock_gpio);
    assert(success);  // clock pin not valid
}

DMAMultiSPI::~DMAMultiSPI() {
    UncachedMemBlock_free(&alloced_);
    free(gpio_shadow_);
}

static int bytes_to_gpio_ops(size_t bytes) {
    // We need two GPIO-operations to bit-bang transfer one bit: one to set the
    // data and one to create a positive clock-edge.
    // For each byte, we have 8 bits.
    // Also, we need a single operation at the end of everything to set clk low.
    return bytes * 8 * 2 + 1;
}

bool DMAMultiSPI::RegisterDataGPIO(int gpio, size_t requested_bytes) {
    if (gpio_dma_ != NULL) {
        fprintf(stderr, "Can not register DataGPIO after SendBuffers() has been"
                "called\n");
        assert(0);
    }
    if (requested_bytes > serial_byte_size_) {
        const int prev_gpio_end = bytes_to_gpio_ops(serial_byte_size_) - 1;
        serial_byte_size_ = requested_bytes;
        const int gpio_operations = bytes_to_gpio_ops(serial_byte_size_);
        gpio_buffer_size_ = gpio_operations * sizeof(GPIOData);
        // We keep an in-memory buffer that we directly manipulate in
        // SetBufferedByte() operations and then copy to the DMA managed buffer
        // when actually sending. Reason is, that the DMA buffer is uncached
        // memory and very slow to access in particular for the operations
        // needed in SetBufferedByte().
        // RegisterDataGPIO() can be called multiple times with different sizes,
        // so we need to be prepared to adjust size.
        gpio_shadow_ = (GPIOData*)realloc(gpio_shadow_, gpio_buffer_size_);
        bzero(gpio_shadow_ + prev_gpio_end,
              (gpio_operations - prev_gpio_end)*sizeof(GPIOData));
        // Prepare every other element to set the CLK pin so that later, we
        // only have to set the data.
        // Even: data, clock low; Uneven: clock pos edge
        for (int i = prev_gpio_end; i < gpio_operations; ++i) {
            if (i % 2 == 0)
                gpio_shadow_[i].clr = (1<<clock_gpio_);
            else
                gpio_shadow_[i].set = (1<<clock_gpio_);
        }
    }

    return gpio_.AddOutput(gpio);
}

void DMAMultiSPI::FinishRegistration() {
    assert(alloced_.mem == NULL);  // Registered twice ?
    // One DMA operation can only span a limited amount of range.
    const int kMaxOpsPerBlock = (2<<15) / sizeof(GPIOData);
    const int gpio_operations = bytes_to_gpio_ops(serial_byte_size_);
    const int control_blocks
        = (gpio_operations + kMaxOpsPerBlock - 1) / kMaxOpsPerBlock;
    const int alloc_size = (control_blocks * sizeof(struct dma_cb)
                            + gpio_operations * sizeof(GPIOData));
    alloced_ = UncachedMemBlock_alloc(alloc_size);
    gpio_dma_ = (struct GPIOData*) ((uint8_t*)alloced_.mem
                                    + control_blocks * sizeof(struct dma_cb));

    struct dma_cb* previous = NULL;
    struct dma_cb* cb = NULL;
    struct GPIOData *start_gpio = gpio_dma_;
    int remaining = gpio_operations;
    for (int i = 0; i < control_blocks; ++i) {
        cb = (struct dma_cb*) ((uint8_t*)alloced_.mem + i * sizeof(dma_cb));
        if (previous) {
            previous->next = UncachedMemBlock_to_physical(&alloced_, cb);
        }
        const int n = remaining > kMaxOpsPerBlock ? kMaxOpsPerBlock : remaining;
        cb->info   = (DMA_CB_TI_SRC_INC | DMA_CB_TI_DEST_INC |
                      DMA_CB_TI_NO_WIDE_BURSTS | DMA_CB_TI_TDMODE);
        cb->src    = UncachedMemBlock_to_physical(&alloced_, start_gpio);
        cb->dst    = PHYSICAL_GPIO_BUS + GPIO_SET_OFFSET;
        cb->length = DMA_CB_TXFR_LEN_YLENGTH(n)
            | DMA_CB_TXFR_LEN_XLENGTH(sizeof(GPIOData));
        cb->stride = DMA_CB_STRIDE_D_STRIDE(-16) | DMA_CB_STRIDE_S_STRIDE(0);
        previous = cb;
        start_gpio += n;
        remaining -= n;
    }
    cb->next = 0;

    // First block in our chain.
    start_block_ = (struct dma_cb*) alloced_.mem;

    // 4.2.1.2
    char *dmaBase = (char*) ft::mmap_bcm_register(DMA_BASE);
    dma_channel_ = (struct dma_channel_header*)(dmaBase + 0x100 * DMA_CHANNEL);
}

void DMAMultiSPI::SetBufferedByte(int data_gpio, size_t pos, uint8_t data) {
    assert(pos < serial_byte_size_);
    GPIOData *buffer_pos = gpio_shadow_ + 2 * 8 * pos;
    for (uint8_t bit = 0x80; bit; bit >>= 1, buffer_pos += 2) {
        if (data & bit) {   // set
            buffer_pos->set |= (1 << data_gpio);
            buffer_pos->clr &= ~(1 << data_gpio);
        } else {            // reset
            buffer_pos->set &= ~(1 << data_gpio);
            buffer_pos->clr |= (1 << data_gpio);
        }
    }
}

void DMAMultiSPI::SendBuffers() {
    if (!gpio_dma_) FinishRegistration();
    memcpy(gpio_dma_, gpio_shadow_, gpio_buffer_size_);

    dma_channel_->cs |= DMA_CS_END;
    dma_channel_->cblock = UncachedMemBlock_to_physical(&alloced_, start_block_);
    dma_channel_->cs = DMA_CS_PRIORITY(7) | DMA_CS_PANIC_PRIORITY(7) | DMA_CS_DISDEBUG;
    dma_channel_->cs |= DMA_CS_ACTIVE;
    while ((dma_channel_->cs & DMA_CS_ACTIVE)
           && !(dma_channel_->cs & DMA_CS_ERROR)) {
        usleep(10);
    }

    dma_channel_->cs |= DMA_CS_ABORT;
    usleep(100);
    dma_channel_->cs &= ~DMA_CS_ACTIVE;
    dma_channel_->cs |= DMA_CS_RESET;
}


// Public interface
MultiSPI *CreateDMAMultiSPI(int clock_gpio) {
    return new DMAMultiSPI(clock_gpio);
}
}  // namespace spixels
