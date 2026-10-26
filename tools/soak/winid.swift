// Find the window a process shows, for `screencapture -l` (tools/soak/soak.sh, S-13).
// Usage: winid PID
// Prints "<window number> <percent covered>" for the first on-screen, normal-layer
// window owned by PID, and exits 0; exits 1 if there is none. The percentage is
// how much of the window lies under normal-layer windows in front of it (summed,
// capped at 100). macOS throttles a covered window, so a static frame while it is
// covered may be the desktop, not the game.
import CoreGraphics
import Foundation

guard CommandLine.arguments.count == 2, let pid = Int32(CommandLine.arguments[1]) else {
    FileHandle.standardError.write("usage: winid PID\n".data(using: .utf8)!)
    exit(2)
}
func bounds(_ w: [String: Any]) -> CGRect? {
    guard let b = w[kCGWindowBounds as String] as? [String: Any],
          let x = (b["X"] as? NSNumber)?.doubleValue, let y = (b["Y"] as? NSNumber)?.doubleValue,
          let wd = (b["Width"] as? NSNumber)?.doubleValue,
          let ht = (b["Height"] as? NSNumber)?.doubleValue else { return nil }
    return CGRect(x: x, y: y, width: wd, height: ht)
}
let opts: CGWindowListOption = [.optionOnScreenOnly, .excludeDesktopElements]
guard let list = CGWindowListCopyWindowInfo(opts, kCGNullWindowID) as? [[String: Any]] else {
    exit(1)
}
var inFront: [CGRect] = []   // the list runs front to back
for w in list {
    guard (w[kCGWindowLayer as String] as? Int) == 0, let r = bounds(w) else { continue }
    if (w[kCGWindowOwnerPID as String] as? Int32) == pid,
       let num = w[kCGWindowNumber as String] as? Int, r.width >= 64, r.height >= 64 {
        var covered = 0.0
        for f in inFront {
            let i = f.intersection(r)
            if !i.isNull { covered += Double(i.width * i.height) }
        }
        print(num, min(100, Int(covered * 100 / Double(r.width * r.height))))
        exit(0)
    }
    if (w[kCGWindowAlpha as String] as? NSNumber)?.doubleValue ?? 1 > 0 { inFront.append(r) }
}
exit(1)
