// HORI 휠(VID 0x0f0d)의 원시 HID 입력 리포트를 읽어, 바뀐 바이트만 출력한다.
import Foundation
import IOKit.hid

let vendorID = 0x0f0d
let duration = Double(CommandLine.arguments.dropFirst().first ?? "60") ?? 60

func hex(_ bytes: [UInt8]) -> String { bytes.map { String(format: "%02x", $0) }.joined(separator: " ") }
func log(_ s: String) {
    let f = DateFormatter(); f.dateFormat = "HH:mm:ss.SSS"
    print("\(f.string(from: Date())) \(s)"); fflush(stdout)
}

var last: [UInt8] = []
var baseline: [UInt8]? = nil
var minV: [UInt8] = [], maxV: [UInt8] = []
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 1024)

let manager = IOHIDManagerCreate(kCFAllocatorDefault, IOOptionBits(kIOHIDOptionsTypeNone))
IOHIDManagerSetDeviceMatching(manager, [kIOHIDVendorIDKey: vendorID] as CFDictionary)

IOHIDManagerRegisterDeviceMatchingCallback(manager, { _, _, _, device in
    let name = IOHIDDeviceGetProperty(device, kIOHIDProductKey as CFString) as? String ?? "?"
    let pid = IOHIDDeviceGetProperty(device, kIOHIDProductIDKey as CFString) as? Int ?? 0
    let page = IOHIDDeviceGetProperty(device, kIOHIDPrimaryUsagePageKey as CFString) as? Int ?? 0
    let usage = IOHIDDeviceGetProperty(device, kIOHIDPrimaryUsageKey as CFString) as? Int ?? 0
    log(String(format: "CONNECTED %@ pid=0x%04x usagePage=0x%04x usage=0x%02x", name, pid, page, usage))
    if let desc = IOHIDDeviceGetProperty(device, kIOHIDReportDescriptorKey as CFString) as? Data {
        log("REPORT DESCRIPTOR (\(desc.count) bytes): \(hex([UInt8](desc)))")
    }
    for key in [kIOHIDMaxInputReportSizeKey, kIOHIDMaxOutputReportSizeKey, kIOHIDMaxFeatureReportSizeKey] {
        log("\(key) = \(IOHIDDeviceGetProperty(device, key as CFString) as? Int ?? -1)")
    }
    IOHIDDeviceRegisterInputReportCallback(device, buf, 1024, { _, _, _, _, reportID, report, length in
        let bytes = Array(UnsafeBufferPointer(start: report, count: length))
        if baseline == nil {
            baseline = bytes; minV = bytes; maxV = bytes
            log("FIRST id=\(reportID) len=\(length): \(hex(bytes))")
        }
        for i in 0..<min(bytes.count, minV.count) { minV[i] = min(minV[i], bytes[i]); maxV[i] = max(maxV[i], bytes[i]) }
        if bytes != last {
            let changed = bytes.indices.filter { $0 >= last.count || bytes[$0] != last[$0] }
            log("id=\(reportID) \(hex(bytes))  changed@\(changed)")
            last = bytes
        }
    }, nil)
}, nil)

IOHIDManagerRegisterDeviceRemovalCallback(manager, { _, _, _, _ in log("DISCONNECTED") }, nil)
IOHIDManagerScheduleWithRunLoop(manager, CFRunLoopGetCurrent(), CFRunLoopMode.defaultMode.rawValue)
let r = IOHIDManagerOpen(manager, IOOptionBits(kIOHIDOptionsTypeNone))
log(String(format: "open result=0x%08x, listening %.0fs", r, duration))
if r != kIOReturnSuccess { log("open 실패 — 입력 모니터링 권한이 필요할 수 있음") }

CFRunLoopRunInMode(CFRunLoopMode.defaultMode, duration, false)
if let b = baseline {
    log("SUMMARY bytes that moved (index: min..max):")
    for i in b.indices where minV[i] != maxV[i] { log(String(format: "  [%2d] 0x%02x..0x%02x", i, minV[i], maxV[i])) }
} else {
    log("SUMMARY: 입력 리포트를 하나도 못 받음")
}
