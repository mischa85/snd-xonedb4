import SwiftUI

struct PloytecAppView: View {
	@StateObject var viewModel = PloytecAppViewModel()
	var userClient = PloytecAppUserClient()

	@State private var userClientText = ""
	@State private var firmwareVersionText = ""
	@State private var deviceNameText = ""
	@State private var deviceManufacturerText = ""
	@State private var playbackStatsText = ""

	private let playbackStatsUpdateInterval = 1.0

	@State private var selectedUrbCount = 4
	private let urbCount = [1, 2, 3, 4, 5, 6, 7, 8]
	
	@State private var usbPCMoutFramesCountOld: UInt64 = 0
	@State private var usbPCMinFramesCountOld: UInt64 = 0
	
	struct FrameCount: Hashable {
		let output: Int
		let input: Int

		var label: String { "\(output)/\(input)" }
	}
	
	@State private var selectedFramesCount = FrameCount(output: 80, input: 80)

	/*
	private let framesCount: [FrameCount] = [
		.init(output: 20, input: 16),
		.init(output: 40, input: 32),
		.init(output: 80, input: 64),
		.init(output: 160, input: 128),
		.init(output: 320, input: 256),
		.init(output: 640, input: 512),
		.init(output: 1280, input: 1024),
		.init(output: 2560, input: 2048),
	]
	*/

	private let framesCount: [FrameCount] = [
		.init(output: 20, input: 16),
		.init(output: 80, input: 80),
		.init(output: 160, input: 160),
		.init(output: 320, input: 320),
		.init(output: 640, input: 640),
		.init(output: 1280, input: 1280),
		.init(output: 2560, input: 2560),
	]

	@State private var timer: Timer?
	@State private var retryTimer: Timer?

	private var urbBinding: Binding<Int> {
		Binding(
			get: { selectedUrbCount },
			set: { newValue in
				if selectedUrbCount != newValue {
					selectedUrbCount = newValue
					userClient.setCurrentUrbCount(UInt8(newValue))
				}
			}
		)
	}
	private var framesBinding: Binding<FrameCount> {
		Binding(
			get: { selectedFramesCount },
			set: { newValue in
				selectedFramesCount = newValue
				userClient.setFrameCount(UInt16(newValue.input), output: UInt16(newValue.output))
			}
		)
	}

	var body: some View {
		ScrollView {
			VStack {
				VStack(alignment: .center) {
					Text("Driver Manager")
						.padding()
						.font(.title)
					Text(self.viewModel.dextLoadingState)
						.multilineTextAlignment(.center)
						.padding()
					HStack {
						Button(action: {
							self.viewModel.activateMyDext()
						}, label: {
							Text("Install Dext")
						})
						Button(action: {
							self.viewModel.deactivateMyDext()
						}, label: {
							Text("Uninstall Dext")
						})
					}
					.padding()
				}
				.frame(width: 500, alignment: .center)
				Spacer()
				VStack(alignment: .center) {
					if viewModel.isConnected {
						Text(deviceNameText)
						Text(deviceManufacturerText)
						Text(firmwareVersionText)
						Text("USB Settings")
							.padding()
						Picker("URB Count", selection: urbBinding) {
							ForEach(urbCount, id: \.self) { size in
								Text("\(size)")
							}
						}
						.frame(width: 150)
						Picker("Frames (Output/Input)", selection: framesBinding) {
							ForEach(framesCount, id: \.self) { pair in
								Text(pair.label)
							}
						}
						.pickerStyle(MenuPickerStyle())
						.frame(width: 250)
					}
					Text("Driver Statistics")
						.padding()
					Text(playbackStatsText)
						.font(.system(.body, design: .monospaced))
						.multilineTextAlignment(.leading)
						.frame(minWidth: 400, maxWidth: 400, minHeight: 250, idealHeight: 250, maxHeight: 250)
						.border(Color.gray)
						.padding()
				}
				.frame(width: 500, alignment: .center)
			}
			Text(viewModel.isConnected ? "Connected" : "Not Connected")
				.font(.headline)
				.padding()
				.foregroundColor(viewModel.isConnected ? .green : .red)
			.onReceive(viewModel.$isConnected) { isConnected in
				if isConnected {
					stopRetryingUserClient()
					firmwareVersionText = self.userClient.getFirmwareVersion()
					deviceNameText = self.userClient.getDeviceName()
					deviceManufacturerText = self.userClient.getDeviceManufacturer()
					selectedUrbCount = Int(userClient.getCurrentUrbCount())
					let input = Int(userClient.getCurrentInputFramesCount())
					let output = Int(userClient.getCurrentOutputFramesCount())
					selectedFramesCount = FrameCount(output: output, input: input)
					startTimer()
				} else {
					stopTimer()
					startRetryingUserClient()
				}
			}
		}
	}
	
	private func startRetryingUserClient() {
		stopRetryingUserClient()
		userClientText = userClient.openConnection()
		retryTimer = Timer.scheduledTimer(withTimeInterval: 5.0, repeats: true) { _ in
			print("Retrying user client connection...")
			userClientText = userClient.openConnection()
		}
	}
	
	private func stopRetryingUserClient() {
		retryTimer?.invalidate()
		retryTimer = nil
	}

	private func startTimer() {
		timer = Timer.scheduledTimer(withTimeInterval: self.playbackStatsUpdateInterval, repeats: true) { _ in
			self.updatePlaybackStats()
		}
	}

	private func stopTimer() {
		timer?.invalidate()
		timer = nil
	}

	private func updatePlaybackStats() {
		let stats = self.userClient.getPlaybackStats()
		self.playbackStatsText =
			"Playback                 : \(stats.playing)\n" +
			"Capture                  : \(stats.recording)\n" +
			"CoreAudio Sampletime Out : \(stats.out_sample_time)\n" +
			"CoreAudio Sampletime In  : \(stats.in_sample_time)\n" +
			"USB PCM Frames Out       : \(stats.usbPCMoutFramesCount)\n" +
			"USB PCM Framerate Out    : \(stats.usbPCMoutFramesCount - usbPCMoutFramesCountOld)\n" +
			"USB PCM Frames In        : \(stats.usbPCMinFramesCount)\n" +
			"USB PCM Framerate In     : \(stats.usbPCMinFramesCount - usbPCMinFramesCountOld)\n" +
			"USB MIDI Bytes Out       : \(stats.usbMIDIoutBytesCount)\n" +
			"USB MIDI Bytes In        : \(stats.usbMIDIinBytesCount)\n" +
			"XRUNS                    : \(stats.xruns)"
		usbPCMoutFramesCountOld = stats.usbPCMoutFramesCount
		usbPCMinFramesCountOld = stats.usbPCMinFramesCount
	}
}

struct PloytecAppView_Previews: PreviewProvider {
	static var previews: some View {
		PloytecAppView()
	}
}
