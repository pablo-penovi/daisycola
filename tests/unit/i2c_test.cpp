// Phase 3: I2C transfers to a host device model.
#include <cstring>
#include <gtest/gtest.h>

#include "daisy_seed.h"
#include "daisycola/host.h"

using namespace daisy;

namespace
{
// A device with a register file: a write sets the register pointer and stores any data bytes,
// a read returns registers from the pointer on.
class FakeChip : public daisycola::I2CDevice
{
  public:
    uint8_t regs[32] = {};
    uint8_t ptr      = 0;
    int     writes = 0, reads = 0;

    bool Write(const uint8_t* data, size_t size) override
    {
        writes++;
        ptr = data[0];
        for(size_t i = 1; i < size; i++)
            regs[(ptr + i - 1) % 32] = data[i];
        return true;
    }
    bool Read(uint8_t* data, size_t size) override
    {
        reads++;
        for(size_t i = 0; i < size; i++)
            data[i] = regs[(ptr + i) % 32];
        return true;
    }
};

struct Done
{
    bool              called = false;
    I2CHandle::Result result = I2CHandle::Result::OK;
};

void OnDone(void* context, I2CHandle::Result result)
{
    auto* done   = static_cast<Done*>(context);
    done->called = true;
    done->result = result;
}

I2CHandle MakeBus()
{
    I2CHandle::Config cfg;
    cfg.mode           = I2CHandle::Config::Mode::I2C_MASTER;
    cfg.periph         = I2CHandle::Config::Peripheral::I2C_1;
    cfg.speed          = I2CHandle::Config::Speed::I2C_100KHZ;
    cfg.pin_config.scl = seed::D11;
    cfg.pin_config.sda = seed::D12;
    I2CHandle i2c;
    i2c.Init(cfg);
    return i2c;
}

class I2C : public ::testing::Test
{
  protected:
    void SetUp() override { daisycola::UseManualClock(true); }
};
} // namespace

TEST_F(I2C, RoundTripThroughBlockingWriteAndDmaRead)
{
    FakeChip chip;
    daisycola::AttachI2CDevice(0, 0x3f, &chip);
    I2CHandle i2c = MakeBus();

    uint8_t write[] = {0x0c, 0x51, 0x52};
    const uint32_t before = System::GetUs();
    ASSERT_EQ(i2c.TransmitBlocking(0x3f, write, 3, 200), I2CHandle::Result::OK);
    EXPECT_EQ(chip.regs[0x0c], 0x51);
    EXPECT_EQ(chip.regs[0x0d], 0x52);
    EXPECT_GE(System::GetUs() - before, 360u); // 4 bytes at 100 kHz

    uint8_t point[] = {0x0c};
    ASSERT_EQ(i2c.TransmitBlocking(0x3f, point, 1, 200), I2CHandle::Result::OK);

    // TAPE ORs 0x80 into the address for reads; the bus only carries the low seven bits.
    uint8_t buf[2] = {};
    Done    done;
    ASSERT_EQ(i2c.ReceiveDma(0x3f | 0x80, buf, 2, OnDone, &done), I2CHandle::Result::OK);
    EXPECT_FALSE(done.called) << "completes in the DMA interrupt, not inside ReceiveDma";
    daisycola::AdvanceClock(100);
    EXPECT_FALSE(done.called) << "3 bytes at 100 kHz take 270 us";
    daisycola::AdvanceClock(300);
    ASSERT_TRUE(done.called);
    EXPECT_EQ(done.result, I2CHandle::Result::OK);
    EXPECT_EQ(buf[0], 0x51);
    EXPECT_EQ(buf[1], 0x52);
}

// CHOMPI's battery check starts a DMA read and writes a register straight after. On the chip the
// write waits for the read, so the read sees the registers as they were.
TEST_F(I2C, BlockingWriteWaitsForARunningDmaRead)
{
    FakeChip chip;
    chip.regs[0x11] = 0xaa;
    daisycola::AttachI2CDevice(0, 0x3f, &chip);
    I2CHandle i2c = MakeBus();

    uint8_t point[] = {0x11};
    ASSERT_EQ(i2c.TransmitBlocking(0x3f, point, 1, 200), I2CHandle::Result::OK);
    uint8_t buf[6] = {};
    Done    done;
    ASSERT_EQ(i2c.ReceiveDma(0x3f | 0x80, buf, 6, OnDone, &done), I2CHandle::Result::OK);

    uint8_t write[] = {0x11, 0x55};
    ASSERT_EQ(i2c.TransmitBlocking(0x3f, write, 2, 200), I2CHandle::Result::OK);
    EXPECT_TRUE(done.called) << "the read finished first";
    EXPECT_EQ(buf[0], 0xaa);
    EXPECT_EQ(chip.regs[0x11], 0x55);
}

TEST_F(I2C, MissingDeviceNacks)
{
    I2CHandle i2c = MakeBus();
    uint8_t   b[] = {0};
    EXPECT_EQ(i2c.TransmitBlocking(0x20, b, 1, 200), I2CHandle::Result::ERR);

    Done done;
    ASSERT_EQ(i2c.ReceiveDma(0x20, b, 1, OnDone, &done), I2CHandle::Result::OK);
    daisycola::AdvanceClock(1000);
    ASSERT_TRUE(done.called);
    EXPECT_EQ(done.result, I2CHandle::Result::ERR);
}

TEST_F(I2C, CallbackRunsAsAnInterruptAndRespectsPrimask)
{
    FakeChip chip;
    daisycola::AttachI2CDevice(0, 0x3f, &chip);
    I2CHandle i2c = MakeBus();
    uint8_t   b[1];
    Done      done;
    __disable_irq();
    i2c.ReceiveDma(0x3f, b, 1, OnDone, &done);
    daisycola::AdvanceClock(1000);
    EXPECT_FALSE(done.called);
    __enable_irq();
    daisycola::ServiceInterrupts();
    EXPECT_TRUE(done.called);
}
