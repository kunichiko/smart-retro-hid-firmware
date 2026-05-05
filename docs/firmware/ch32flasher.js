// ===================================================================================
// CH32Flasher.js - WebUSB Firmware Flasher for WCH CH32 Microcontrollers
// ===================================================================================
// Based on chprog by Stefan Wagner (https://github.com/wagiminator/MCU-Flash-Tools)
// and CH559Flasher.js by toyoshim (https://github.com/toyoshim/CH559Flasher.js)
//
// License: MIT
// ===================================================================================

const CH_USB_VENDOR_ID1 = 0x4348;
const CH_USB_VENDOR_ID2 = 0x1a86;
const CH_USB_PRODUCT_ID = 0x55e0;
const CH_USB_EP_OUT = 0x02;
const CH_USB_EP_IN = 0x02; // transferIn uses endpoint number without direction bit
const CH_USB_PACKET_SIZE = 64;
const CH_XOR_KEY_LEN = 8;

const CH_CMD_CHIP_DETECT = 0xa1;
const CH_CMD_REBOOT = 0xa2;
const CH_CMD_KEY_SET = 0xa3;
const CH_CMD_CODE_ERASE = 0xa4;
const CH_CMD_CODE_WRITE = 0xa5;
const CH_CMD_CODE_VERIFY = 0xa6;
const CH_CMD_CONFIG_READ = 0xa7;

const CH_STR_CHIP_DETECT = new Uint8Array([
    0xa1, 0x12, 0x00, 0x52, 0x11,
    0x4d, 0x43, 0x55, 0x20, 0x49, 0x53, 0x50, 0x20,  // "MCU ISP "
    0x26, 0x20, 0x57, 0x43, 0x48, 0x2e, 0x43, 0x4e    // "& WCH.CN"
]);
const CH_STR_CONFIG_READ = new Uint8Array([0xa7, 0x02, 0x00, 0x1f, 0x00]);
const CH_STR_REBOOT = new Uint8Array([0xa2, 0x01, 0x00, 0x01]);

// Device database (CH32X035 variants)
const DEVICES = [
    { name: 'CH32X033F8P6', id: 0x235a, code_size: 63488 },
    { name: 'CH32X035R8T6', id: 0x2350, code_size: 63488 },
    { name: 'CH32X035C8T6', id: 0x2351, code_size: 63488 },
    { name: 'CH32X035G8U6', id: 0x2356, code_size: 63488 },
    { name: 'CH32X035F7P6', id: 0x2357, code_size: 49152 },
    { name: 'CH32X035G8R6', id: 0x235b, code_size: 63488 },
    { name: 'CH32X035F8U6', id: 0x235e, code_size: 63488 },
    // Add more CH32 devices here as needed
];

const LASTWRITE_FAMILIES = [0x12, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19, 0x20, 0x22, 0x23, 0x24, 0x25];

export class CH32Flasher {
    constructor() {
        this.device = null;
        this.usbDevice = null;
        this.xorkey = new Uint8Array(CH_XOR_KEY_LEN);
        this.chipName = '';
        this.bootloader = '';
        this.codeFlashSize = 0;
        this.lastwrite = false;
    }

    // Connect to the device via WebUSB
    async connect() {
        this.usbDevice = await navigator.usb.requestDevice({
            filters: [
                { vendorId: CH_USB_VENDOR_ID1, productId: CH_USB_PRODUCT_ID },
                { vendorId: CH_USB_VENDOR_ID2, productId: CH_USB_PRODUCT_ID },
            ]
        });

        await this.usbDevice.open();
        await this.usbDevice.selectConfiguration(1);
        await this.usbDevice.claimInterface(0);
    }

    // Send a command and receive a reply
    async sendCommand(data) {
        const txResult = await this.usbDevice.transferOut(CH_USB_EP_OUT, data);
        if (txResult.status !== 'ok') {
            throw new Error(`USB transferOut failed: ${txResult.status}`);
        }
        const rxResult = await this.usbDevice.transferIn(CH_USB_EP_IN, CH_USB_PACKET_SIZE);
        if (rxResult.status !== 'ok') {
            throw new Error(`USB transferIn failed: ${rxResult.status}`);
        }
        return new Uint8Array(rxResult.data.buffer);
    }

    // Detect and identify the chip
    async detect() {
        // Detect chip type
        const identReply = await this.sendCommand(CH_STR_CHIP_DETECT);
        if (identReply.length !== 6) {
            throw new Error('Failed to identify chip');
        }

        const chipType = identReply[4];
        const chipFamily = identReply[5];
        const chipId = (chipFamily << 8) + chipType;

        // Read configuration
        const cfgReply = await this.sendCommand(CH_STR_CONFIG_READ);
        if (cfgReply.length !== 30) {
            throw new Error('Failed to read chip configuration');
        }

        this.bootloader = `${cfgReply[19]}.${cfgReply[20]}.${cfgReply[21]}`;

        // Find device in database
        this.device = DEVICES.find(d => d.id === chipId);
        if (!this.device) {
            throw new Error(`Unsupported chip (ID: 0x${chipId.toString(16).padStart(4, '0')})`);
        }

        this.chipName = this.device.name;
        this.codeFlashSize = this.device.code_size;
        this.lastwrite = LASTWRITE_FAMILIES.includes(chipFamily);

        // Determine UID length
        const uidLen = chipFamily > 0x11 ? 8 : 4;
        const chipUid = cfgReply.slice(22, 22 + uidLen);

        // Create XOR encryption key
        let sum = 0;
        for (let i = 0; i < uidLen; i++) {
            sum += chipUid[i];
        }
        for (let i = 0; i < CH_XOR_KEY_LEN - 1; i++) {
            this.xorkey[i] = sum & 0xff;
        }
        this.xorkey[CH_XOR_KEY_LEN - 1] = (sum + chipType) & 0xff;

        // Send encryption key to device
        let keySum = 0;
        for (let i = 0; i < CH_XOR_KEY_LEN; i++) {
            keySum += this.xorkey[i];
        }
        const keyCmd = new Uint8Array(3 + 0x1e);
        keyCmd[0] = CH_CMD_KEY_SET;
        keyCmd[1] = 0x1e;
        keyCmd[2] = 0x00;
        const keyReply = await this.sendCommand(keyCmd);
        if (keyReply[4] !== (keySum & 0xff)) {
            throw new Error('Failed to set encryption key');
        }

        // Unlock chip if needed (remove write protection)
        const configData = cfgReply.slice(6, 18);
        if (configData[0] === 0xff) {
            configData[0] = 0xa5;
            const unlockCmd = new Uint8Array([0xa8, 0x0e, 0x00, 0x07, 0x00, ...configData]);
            await this.sendCommand(unlockCmd);
        }

        return { chipName: this.chipName, bootloader: this.bootloader };
    }

    // Erase code flash
    async erase(size) {
        if (size > this.codeFlashSize) {
            throw new Error('Firmware too large for this chip');
        }
        let sectors = Math.floor((size + 1023) / 1024);
        if (sectors < 8) sectors = 8;
        const cmd = new Uint8Array([
            CH_CMD_CODE_ERASE, 0x04, 0x00,
            sectors & 0xff, (sectors >> 8) & 0xff, 0x00, 0x00
        ]);
        const reply = await this.sendCommand(cmd);
        if (reply[4] !== 0x00) {
            throw new Error('Failed to erase chip');
        }
    }

    // Write firmware data to flash
    async write(data, progressCallback) {
        await this.erase(data.length);
        await this._writeVerify(data, CH_CMD_CODE_WRITE, progressCallback);
    }

    // Verify firmware data in flash
    async verify(data, progressCallback) {
        await this._writeVerify(data, CH_CMD_CODE_VERIFY, progressCallback);
    }

    // Internal: write or verify flash data
    async _writeVerify(data, mode, progressCallback) {
        // Pad data to XOR key length
        let padded = new Uint8Array(data.length + (CH_XOR_KEY_LEN - (data.length % CH_XOR_KEY_LEN)) % CH_XOR_KEY_LEN);
        padded.set(data);
        for (let i = data.length; i < padded.length; i++) {
            padded[i] = 0xff;
        }

        // XOR encrypt
        const encrypted = new Uint8Array(padded.length);
        for (let i = 0; i < padded.length; i++) {
            encrypted[i] = padded[i] ^ this.xorkey[i % CH_XOR_KEY_LEN];
        }

        // Send data in chunks of 56 bytes
        const totalLen = encrypted.length;
        let offset = 0;
        while (offset < totalLen) {
            const chunkLen = Math.min(totalLen - offset, 0x38);
            const remaining = totalLen - offset;
            const cmd = new Uint8Array(3 + 4 + 1 + chunkLen);
            cmd[0] = mode;
            cmd[1] = chunkLen + 5;
            cmd[2] = 0x00;
            // offset (little-endian 32-bit)
            cmd[3] = offset & 0xff;
            cmd[4] = (offset >> 8) & 0xff;
            cmd[5] = (offset >> 16) & 0xff;
            cmd[6] = (offset >> 24) & 0xff;
            // remaining
            cmd[7] = remaining & 0xff;
            // data chunk
            cmd.set(encrypted.slice(offset, offset + chunkLen), 8);

            const reply = await this.sendCommand(cmd);
            if (reply[4] !== 0x00 && reply[4] !== 0xfe && reply[4] !== 0xf5) {
                const op = mode === CH_CMD_CODE_WRITE ? 'write' : 'verify';
                throw new Error(`Failed to ${op} at offset 0x${offset.toString(16).padStart(8, '0')}`);
            }

            offset += chunkLen;
            if (progressCallback) {
                progressCallback(offset / totalLen);
            }
        }

        // Some chips need a last empty write
        if (this.lastwrite && mode === CH_CMD_CODE_WRITE) {
            const cmd = new Uint8Array([
                mode, 0x05, 0x00,
                offset & 0xff, (offset >> 8) & 0xff, (offset >> 16) & 0xff, (offset >> 24) & 0xff,
                0x00
            ]);
            await this.sendCommand(cmd);
        }
    }

    // Reboot the device
    async reboot() {
        try {
            await this.usbDevice.transferOut(CH_USB_EP_OUT, CH_STR_REBOOT);
        } catch (e) {
            // Device may disconnect before reply
        }
    }

    // Disconnect
    async disconnect() {
        if (this.usbDevice) {
            try {
                await this.usbDevice.releaseInterface(0);
                await this.usbDevice.close();
            } catch (e) {
                // Ignore
            }
            this.usbDevice = null;
        }
    }
}
