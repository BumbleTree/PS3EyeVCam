// source code from https://github.com/inspirit/PS3EYEDriver
#ifndef PS3EYECAM_H
#define PS3EYECAM_H

#include <memory>
#include <vector>

struct libusb_device;
struct libusb_device_handle;

#ifndef __STDC_CONSTANT_MACROS
#  define __STDC_CONSTANT_MACROS
#endif

#include <stdint.h>

namespace ps3eye {

class PS3EYECam
{
public:
	enum class EOutputFormat
	{
		Bayer,					// Output in Bayer. Destination buffer must be width * height bytes
		BGR,					// Output in BGR. Destination buffer must be width * height * 3 bytes
		RGB	,					// Output in RGB. Destination buffer must be width * height * 3 bytes
		BGRA,					// Output in BGRA. Destination buffer must be width * height * 4 bytes
		RGBA,					// Output in RGBA. Destination buffer must be width * height * 4 bytes
		Gray,					// Output in Grayscale. Destination buffer must be width * height bytes
		YUY2					// Output in YUY2 (YUYV 4:2:2, BT.601 limited range), debayered in a
								// single fused pass. Destination buffer must be width * height * 2 bytes
								// (PS3EyeVCam patch)
	};

	typedef std::shared_ptr<PS3EYECam> PS3EYERef;

	static const uint16_t VENDOR_ID;
	static const uint16_t PRODUCT_ID;

	PS3EYECam(libusb_device *device);
	~PS3EYECam();

	bool init(uint32_t width = 0, uint32_t height = 0, uint16_t desiredFrameRate = 30, EOutputFormat outputFormat = EOutputFormat::BGR);
	// Returns false when the USB transfers could not be submitted (device
	// gone / driver problem); the camera is left fully stopped in that case
	// (PS3EyeVCam patch: was void and claimed streaming even on failure).
	bool start();
	void stop();
	void release();

	// Controls

	bool getAutogain() const { return autogain; }
	void setAutogain(bool val) {
	    autogain = val;
	    // COM8[2:0] = AGC, AWB, AEC enables. The old 0xF0 manual path also
	    // cleared AWB, fighting setAutoWhiteBalance(); touch only AGC/AEC.
	    uint8_t com8 = sccb_reg_read(0x13);
	    if (val) {
			com8 |= 0x05;                 // AEC + AGC on
			if (awb) com8 |= 0x02;        // preserve AWB when requested
			sccb_reg_write(0x13, com8);
			sccb_reg_write(0x64, sccb_reg_read(0x64) | 0x03);
	    } else {
			com8 &= ~0x05;                // manual exposure/gain
			if (awb) com8 |= 0x02;
			else     com8 &= ~0x02;
			sccb_reg_write(0x13, com8);
			sccb_reg_write(0x64, sccb_reg_read(0x64) & 0xFC);
			setGain(gain);
			setExposure(exposure);
	    }
	}
	bool getAutoWhiteBalance() const { return awb; }
	void setAutoWhiteBalance(bool val) {
	    awb = val;
	    uint8_t com8 = sccb_reg_read(0x13);
	    if (val) {
			com8 |= 0x02;                 // COM8 AWB enable
			sccb_reg_write(0x63, 0xe0);   // AWB_Ctrl0: calculate + apply gains
	    } else {
			com8 &= ~0x02;
			sccb_reg_write(0x63, 0xAA);
	    }
	    sccb_reg_write(0x13, com8);
	}
	uint8_t getGain() const { return gain; }
	void setGain(uint8_t val) {
	    gain = val;
	    switch(val & 0x30){
		case 0x00:
		    val &=0x0F;
		    break;
		case 0x10:
		    val &=0x0F;
		    val |=0x30;
		    break;
		case 0x20:
		    val &=0x0F;
		    val |=0x70;
		    break;
		case 0x30:
		    val &=0x0F;
		    val |=0xF0;
		    break;
	    }
	    sccb_reg_write(0x00, val);
	}
	uint8_t getExposure() const { return exposure; }
	void setExposure(uint8_t val) {
	    exposure = val;
	    sccb_reg_write(0x08, val>>7);
    	sccb_reg_write(0x10, val<<1);
	}
	uint8_t getRedBalance() const { return redblc; }
	void setRedBalance(uint8_t val) {
		redblc = val;
		sccb_reg_write(0x02, val);   // AWB red channel gain (was 0x43 = BLC target)
	}
	uint8_t getBlueBalance() const { return blueblc; }
	void setBlueBalance(uint8_t val) {
		blueblc = val;
		sccb_reg_write(0x01, val);   // AWB blue channel gain (was 0x42 = BLC target)
	}
	uint8_t getGreenBalance() const { return greenblc; }
	void setGreenBalance(uint8_t val) {
		greenblc = val;
		sccb_reg_write(0x03, val);   // AWB green channel gain (was 0x44 = BLC Gb target)
	}
    bool getFlipH() const { return flip_h; }
    bool getFlipV() const { return flip_v; }
	void setFlip(bool horizontal = false, bool vertical = false) {
        flip_h = horizontal;
        flip_v = vertical;
		uint8_t val = sccb_reg_read(0x0c);
        val &= ~0xc0;
        if (!horizontal) val |= 0x40;
        if (!vertical) val |= 0x80;
        sccb_reg_write(0x0c, val);
	}
    
    bool getTestPattern() const { return testPattern; }
    void setTestPattern(bool enable)
    {
        testPattern = enable;
        uint8_t val = sccb_reg_read(0x0C);
        val &= ~0b00000001;
        if (testPattern) val |= 0b00000001; // 0x80;
        sccb_reg_write(0x0C, val);
    }


    bool isStreaming() const { return is_streaming; }
    bool isInitialized() const { return device_ != NULL && handle_ != NULL && usb_buf != NULL; }

    libusb_device *getDevice() const { return device_; }
	bool getUSBPortPath(char *out_identifier, size_t max_identifier_length) const;
	
	// Get a frame from the camera. Notes:
	// - Blocks for up to timeoutMs waiting for a frame; returns false on
	//   timeout or when streaming was stopped/aborted (PS3EyeVCam patch).
	// - The output buffer must be sized correctly, depending out the output format. See EOutputFormat.
	bool getFrame(uint8_t* frame, uint32_t timeoutMs = 500);

	uint32_t getWidth() const { return frame_width; }
	uint32_t getHeight() const { return frame_height; }
	uint16_t getFrameRate() const { return frame_rate; }
	bool setFrameRate(uint8_t val) {
		if (is_streaming) return false;
		frame_rate = ov534_set_frame_rate(val, true);
		return true;
	}
	uint32_t getRowBytes() const { return frame_width * getOutputBytesPerPixel(); }
	uint32_t getOutputBytesPerPixel() const;

	//
	static const std::vector<PS3EYERef>& getDevices( bool forceRefresh = false );

private:
	PS3EYECam(const PS3EYECam&);
    void operator=(const PS3EYECam&);

	// usb ops
	uint16_t ov534_set_frame_rate(uint16_t frame_rate, bool dry_run = false);
	void ov534_set_led(int status);
	void ov534_reg_write(uint16_t reg, uint8_t val);
	uint8_t ov534_reg_read(uint16_t reg);
	int sccb_check_status();
	void sccb_reg_write(uint8_t reg, uint8_t val);
	uint8_t sccb_reg_read(uint16_t reg);
	void reg_w_array(const uint8_t (*data)[2], int len);
	void sccb_w_array(const uint8_t (*data)[2], int len);

	// controls
	bool autogain;
	uint8_t gain; // 0 <-> 63
	uint8_t exposure; // 0 <-> 255
	bool awb;
	uint8_t blueblc; // 0 <-> 255
	uint8_t redblc; // 0 <-> 255
	uint8_t greenblc; // 0 <-> 255
    bool flip_h;
    bool flip_v;
    bool testPattern;
	//
    bool is_streaming;

	std::shared_ptr<class USBMgr> mgrPtr;

	static bool devicesEnumerated;
    static std::vector<PS3EYERef> devices;

	uint32_t frame_width;
	uint32_t frame_height;
	uint16_t frame_rate;
	EOutputFormat frame_output_format;

	//usb stuff
	libusb_device *device_;
	libusb_device_handle *handle_;
	uint8_t *usb_buf;

	std::shared_ptr<class URBDesc> urb;

	bool open_usb();
	void close_usb();

};

} // namespace


#endif
