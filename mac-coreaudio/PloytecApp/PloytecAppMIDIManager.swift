import Foundation
import CoreMIDI

class MIDIManager {
	private var client = MIDIClientRef()
	private var source = MIDIEndpointRef()

	init() {
		let status1 = MIDIClientCreate("PloytecClient" as CFString, nil, nil, &client)
		if status1 != noErr {
			print("MIDIClientCreate failed: \(status1)")
			return
		}

		let status2 = MIDISourceCreate(client, "Ploytec Virtual Output" as CFString, &source)
		if status2 != noErr {
			print("MIDISourceCreate failed: \(status2)")
			return
		}
	}

	func send(bytes: [UInt8]) {
		var packet = MIDIPacket()
		packet.timeStamp = 0
		packet.length = UInt16(bytes.count)
		withUnsafeMutableBytes(of: &packet.data) { rawBuffer in
			rawBuffer.copyBytes(from: bytes)
		}
		var packetList = MIDIPacketList(numPackets: 1, packet: packet)
		MIDIReceived(source, &packetList)
	}
}
