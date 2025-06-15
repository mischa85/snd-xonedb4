import Foundation
import CoreMIDI

class MIDIManager {
	private var client = MIDIClientRef()
	private var source = MIDIEndpointRef()
	private var destination = MIDIEndpointRef()

	weak var userClient: PloytecAppUserClientSwift?

	init() {
		let status1 = MIDIClientCreate("PloytecClient" as CFString, nil, nil, &client)
		guard status1 == noErr else { return }

		let status2 = MIDISourceCreate(client, "Ploytec Virtual Input" as CFString, &source)
		guard status2 == noErr else { return }

		let status3 = MIDIDestinationCreate(client, "Ploytec Virtual Output" as CFString, midiReadCallback,
			UnsafeMutableRawPointer(Unmanaged.passUnretained(self).toOpaque()), &destination)
		if status3 != noErr { return }
	}

	func send(bytes: [UInt8]) {
		var packet = MIDIPacket()
		packet.timeStamp = 0
		packet.length = UInt16(bytes.count)
		withUnsafeMutableBytes(of: &packet.data) { $0.copyBytes(from: bytes) }
		var packetList = MIDIPacketList(numPackets: 1, packet: packet)
		MIDIReceived(source, &packetList)
	}

	private let midiReadCallback: MIDIReadProc = { packetList, refCon, _ in
		let selfInstance = Unmanaged<MIDIManager>.fromOpaque(refCon!).takeUnretainedValue()
		var packet = packetList.pointee.packet

		for _ in 0..<packetList.pointee.numPackets {
			let data = withUnsafeBytes(of: packet.data) { rawBuffer in
				Array(rawBuffer.prefix(Int(packet.length)))
			}

			if data.count == 0 || data.count > 3 {
				print("⚠️ Dropped oversized MIDI message (\(data.count) bytes): \(data.map { String(format: "0x%02X", $0) })")
			} else {
				print("🎵 MIDI: \(data.map { String(format: "0x%02X", $0) })")
				var packed = UInt64(UInt8(data.count))
				for (i, byte) in data.enumerated() {
					packed |= UInt64(byte) << (8 * (i + 1))
				}
				selfInstance.userClient?.sendToDriver(packed)
			}

			packet = MIDIPacketNext(&packet).pointee
		}
	}
}
