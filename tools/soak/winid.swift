// Print the window number of the first on-screen, normal-layer window owned
// by a process, for `screencapture -l` (tools/soak/soak.sh, S-13).
// Usage: winid PID     exit 0 and print the number, or exit 1 if none.
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2, let pid = Int32(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: winid PID\n".data(using: .utf8)!)
    exit(2)
}
let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
guard let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] else {
    exit(1)
}
for w in list {
    guard (w[kCGWindowOwnerPID as String] as? Int32) == pid,
          (w[kCGWindowLayer as String] as? Int) == 0,
          let num = w[kCGWindowNumber as String] as? Int else { continue }
    if let b = w[kCGWindowBounds as String] as? [String: Any],
       let wd = (b["Width"] as? NSNumber)?.doubleValue,
       let ht = (b["Height"] as? NSNumber)?.doubleValue,
       wd < 64 || ht < 64 { continue }
    print(num)
    exit(0)
}
exit(1)
