// The parts of Web Serial (not in TypeScript's DOM types) that usb.ts uses.
interface SerialPortFilter { usbVendorId?: number; usbProductId?: number }
interface SerialPort extends EventTarget {
    readonly readable: ReadableStream<Uint8Array> | null;
    readonly writable: WritableStream<Uint8Array> | null;
    open(options: { baudRate: number }): Promise<void>;
    close(): Promise<void>;
    getInfo(): SerialPortFilter;
}
interface Serial extends EventTarget {
    getPorts(): Promise<SerialPort[]>;
    requestPort(options?: { filters?: SerialPortFilter[] }): Promise<SerialPort>;
}
interface Navigator { readonly serial: Serial }
