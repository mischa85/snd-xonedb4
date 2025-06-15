import Foundation

class PloytecAppUserClientSwift: NSObject {
	private let midiManager = MIDIManager()

	override init() {
		super.init()
		NotificationCenter.default.addObserver(self,
			selector: #selector(handleMIDINotification(_:)),
			name: NSNotification.Name("PloytecMIDIMessageReceived"),
			object: nil)
	}

	@objc private func handleMIDINotification(_ notification: Notification) {
		guard let length = notification.userInfo?["length"] as? UInt8,
			let data = notification.userInfo?["bytes"] as? Data else {
			print("Invalid MIDI notification payload")
			return
		}

		let bytes = [UInt8](data.prefix(Int(length)))

		midiManager.send(bytes: bytes)
	}
}
