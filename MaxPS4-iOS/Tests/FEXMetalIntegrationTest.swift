import Foundation
import Metal
import Darwin

@main
struct FEXMetalIntegrationTest {
    static func main() throws {
        var state = MaxPS4FEXMetalState()
        var offsets = [SIMD2<Float>]()
        var guestDurationsMs = [Double]()
        for _ in 0..<8 {
            for initial in [UInt32(0), 750, 1500] {
                let guestStart = ProcessInfo.processInfo.systemUptime
                let result = try MaxPS4FEXMetalExecution.run(initial: initial)
                guestDurationsMs.append((ProcessInfo.processInfo.systemUptime - guestStart) * 1000)
                precondition(result.backend == UInt32(MAXPS4_CPU_FEX_ARM64_JIT))
                precondition(result.rax == UInt64(initial) + 250)
                try state.accept(result)
                offsets.append(MaxPS4FEXMetalState.objectOffset(mode: 3, touch: .zero, cpu: state.cpuOffset))
                precondition(maxps4_jit_leases() == 0, "native engine must close all leases")
            }
        }
        for bad in [Data(), Data([0x7F]), { () -> Data in
            var file = MaxPS4FEXMetalState.program(initial: 750); file[130] = 0xC3; return file
        }()] {
            do { _ = try MaxPS4SyntheticELFFEXBridge.run(file: bad); preconditionFailure("bad ELF accepted") } catch {}
            precondition(maxps4_jit_leases() == 0)
        }
        let sortedGuestMs = guestDurationsMs.sorted()
        let p50GuestMs = sortedGuestMs[sortedGuestMs.count / 2]
        let p95GuestMs = sortedGuestMs[min(sortedGuestMs.count - 1, Int(Double(sortedGuestMs.count - 1) * 0.95))]
        print(String(format: "PERF guest FEX invocation (macOS CI, n=%d): p50=%.2f ms, p95=%.2f ms", guestDurationsMs.count, p50GuestMs, p95GuestMs))
        print("PASS actual ELF bytes -> FEX ARM64 JIT -> RAX -> bounded renderer offsets; repeated execution/errors and zero leases")
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw NSError(domain: "FEXMetalIntegration", code: 1, userInfo: [NSLocalizedDescriptionKey: "Metal device required for CPU/GPU integration proof"])
        }
        let pipeline = try MaxPS4MetalPipeline.makeTextured(device: device)
        let vertices = try MaxPS4MetalPipeline.makeQuadVertexBuffer(device: device)
        let checkerboard = try MaxPS4MetalPipeline.makeCheckerboard(device: device)
        let desc = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .bgra8Unorm, width: 128, height: 128, mipmapped: false)
        desc.storageMode = .shared; desc.usage = [.renderTarget]
        let target = device.makeTexture(descriptor: desc)!
        let queue = device.makeCommandQueue()!
        func draw(_ position: SIMD2<Float>) throws -> ([UInt8], Double) {
            let pass = MTLRenderPassDescriptor()
            pass.colorAttachments[0].texture = target
            pass.colorAttachments[0].loadAction = .clear
            pass.colorAttachments[0].storeAction = .store
            pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1)
            let command = queue.makeCommandBuffer()!
            let encoder = command.makeRenderCommandEncoder(descriptor: pass)!
            var u = MaxPS4MetalPipeline.uniforms(elapsed: 0, width: 128, height: 128)
            var offset = position
            encoder.setRenderPipelineState(pipeline)
            encoder.setVertexBuffer(vertices, offset: 0, index: 1)
            encoder.setVertexBytes(&u, length: MemoryLayout<SIMD3<Float>>.stride, index: 0)
            encoder.setVertexBytes(&offset, length: MemoryLayout<SIMD2<Float>>.stride, index: 2)
            encoder.setFragmentTexture(checkerboard, index: 0)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 6)
            encoder.endEncoding(); command.commit(); command.waitUntilCompleted()
            if let error = command.error { throw error }
            precondition(command.status == .completed)
            var pixels = [UInt8](repeating: 0, count: 128 * 128 * 4)
            pixels.withUnsafeMutableBytes { target.getBytes($0.baseAddress!, bytesPerRow: 512, from: MTLRegionMake2D(0, 0, 128, 128), mipmapLevel: 0) }
            var sum = 0.0; var count = 0.0
            for y in 0..<128 { for x in 0..<128 {
                if pixels[(y * 128 + x) * 4] != 0 { sum += Double(x); count += 1 }
            } }
            precondition(count > 0)
            return (pixels, sum / count)
        }
        let renderStart = ProcessInfo.processInfo.systemUptime
        let left = try draw(offsets[0]), center = try draw(offsets[1]), right = try draw(offsets[2])
        print(String(format: "PERF Metal 3 offscreen draws + readback (macOS CI): %.2f ms total", (ProcessInfo.processInfo.systemUptime - renderStart) * 1000))
        precondition(left.0 != center.0 && center.0 != right.0)
        precondition(left.1 + 10 < center.1 && center.1 + 10 < right.1)
        print("PASS real Metal completed commands and readback: FEX-computed positions move textured object left/center/right")
        print("macOS ARM64 integration only; authorized physical iPhone validation remains required; no PS4 game")
    }
}
