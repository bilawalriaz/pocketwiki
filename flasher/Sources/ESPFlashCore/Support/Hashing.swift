import Foundation

// Pure-Swift MD5 and SHA-256 so the core has zero dependencies and stays
// portable to Linux/Windows (CryptoKit is Apple-only). Both are used for
// flash verification and image patching, where byte-exactness matters.

public enum MD5 {
    public static func digest(_ input: Data) -> Data {
        var message = [UInt8](input)
        let origLen = UInt64(message.count) * 8

        message.append(0x80)
        while message.count % 64 != 56 { message.append(0) }
        for i in 0..<8 {
            message.append(UInt8((origLen >> (UInt64(i) * 8)) & 0xFF))
        }

        var a0: UInt32 = 0x6745_2301
        var b0: UInt32 = 0xEFCD_AB89
        var c0: UInt32 = 0x98BA_DCFE
        var d0: UInt32 = 0x1032_5476

        let s: [UInt32] = [
            7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
            5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
            4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
            6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
        ]
        let k: [UInt32] = (0..<64).map { UInt32(abs(sin(Double($0 + 1))) * 4_294_967_296.0) }

        for chunkStart in stride(from: 0, to: message.count, by: 64) {
            var m = [UInt32](repeating: 0, count: 16)
            for i in 0..<16 {
                let off = chunkStart + i * 4
                m[i] = UInt32(message[off])
                    | (UInt32(message[off + 1]) << 8)
                    | (UInt32(message[off + 2]) << 16)
                    | (UInt32(message[off + 3]) << 24)
            }
            var a = a0, b = b0, c = c0, d = d0
            for i in 0..<64 {
                let (f, g): (UInt32, Int)
                switch i {
                case 0..<16: f = (b & c) | (~b & d); g = i
                case 16..<32: f = (d & b) | (~d & c); g = (5 * i + 1) % 16
                case 32..<48: f = b ^ c ^ d; g = (3 * i + 5) % 16
                default: f = c ^ (b | ~d); g = (7 * i) % 16
                }
                let temp = d
                d = c
                c = b
                b = b &+ rotateLeft(a &+ f &+ k[i] &+ m[g], by: s[i])
                a = temp
            }
            a0 = a0 &+ a
            b0 = b0 &+ b
            c0 = c0 &+ c
            d0 = d0 &+ d
        }

        var out = Data()
        for v in [a0, b0, c0, d0] {
            for i in 0..<4 {
                out.append(UInt8((v >> (UInt32(i) * 8)) & 0xFF))
            }
        }
        return out
    }

    public static func hex(_ input: Data) -> String {
        digest(input).hex
    }

    private static func rotateLeft(_ v: UInt32, by n: UInt32) -> UInt32 {
        (v << n) | (v >> (32 - n))
    }
}

public enum SHA256 {
    private static let k: [UInt32] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]

    public static func digest(_ input: Data) -> Data {
        var message = [UInt8](input)
        let bitLen = UInt64(message.count) * 8

        message.append(0x80)
        while message.count % 64 != 56 { message.append(0) }
        for i in 0..<8 {
            message.append(UInt8((bitLen >> (56 - UInt64(i) * 8)) & 0xFF))
        }

        var h: [UInt32] = [
            0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
            0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
        ]

        for chunkStart in stride(from: 0, to: message.count, by: 64) {
            var w = [UInt32](repeating: 0, count: 64)
            for i in 0..<16 {
                let off = chunkStart + i * 4
                w[i] = (UInt32(message[off]) << 24)
                    | (UInt32(message[off + 1]) << 16)
                    | (UInt32(message[off + 2]) << 8)
                    | UInt32(message[off + 3])
            }
            for i in 16..<64 {
                let s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3)
                let s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10)
                w[i] = w[i - 16] &+ s0 &+ w[i - 7] &+ s1
            }

            var a = h[0], b = h[1], c = h[2], d = h[3]
            var e = h[4], f = h[5], g = h[6], hh = h[7]

            for i in 0..<64 {
                let s1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)
                let ch = (e & f) ^ (~e & g)
                let temp1 = hh &+ s1 &+ ch &+ k[i] &+ w[i]
                let s0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)
                let maj = (a & b) ^ (a & c) ^ (b & c)
                let temp2 = s0 &+ maj
                hh = g; g = f; f = e; e = d &+ temp1
                d = c; c = b; b = a; a = temp1 &+ temp2
            }

            h[0] &+= a; h[1] &+= b; h[2] &+= c; h[3] &+= d
            h[4] &+= e; h[5] &+= f; h[6] &+= g; h[7] &+= hh
        }

        var out = Data()
        for v in h {
            for i in stride(from: 24, through: 0, by: -8) {
                out.append(UInt8((v >> UInt32(i)) & 0xFF))
            }
        }
        return out
    }

    public static func hex(_ input: Data) -> String {
        digest(input).hex
    }

    private static func rotr(_ v: UInt32, _ n: UInt32) -> UInt32 {
        (v >> n) | (v << (32 - n))
    }
}
