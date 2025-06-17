import Foundation

class PloytecAppUserClientSwift: NSObject {
	let objcClient = PloytecAppUserClient()
	lazy var midiManager: MIDIManager = {
		let name = objcClient.getDeviceName() ?? "Ploytec"
		let manager = MIDIManager(deviceName: name)
		manager.userClient = self
		return manager
	}()

	override init() {
		super.init()

		let result = objcClient.openConnection()
		if let result = result {
			print("Connection status: \(result)")
		} else {
			print("Failed to open connection (nil response)")
		}

		_ = midiManager

		NotificationCenter.default.addObserver(
			self,
			selector: #selector(handleMIDINotification(_:)),
			name: NSNotification.Name("PloytecMIDIMessageReceived"),
			object: nil
		)
	}

	@objc func sendToDriver(_ packed: UInt64) {
		objcClient.sendMIDIMessage(toDriver: packed)
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
