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

	@State private var selectedInputFramesCount = 32
	private let inputFramesCount = [16, 32, 48, 64, 80, 96, 112, 128]

	@State private var selectedOutputFramesCount = 40
	private let outputFramesCount = [10, 20, 30, 40, 50, 60, 70, 80]

	@State private var timer: Timer?
	@State private var retryTimer: Timer?

	private var urbBinding: Binding<Int> {
		Binding(
			get: { selectedUrbCount },
			set: { newValue in
				if selectedUrbCount != newValue {
					selectedUrbCount = newValue
					userClient.changeUrbCount(UInt8(newValue))
				}
			}
		)
	}
	private var inputFramesBinding: Binding<Int> {
		Binding(
			get: { selectedInputFramesCount },
			set: { newValue in
				if selectedInputFramesCount != newValue {
					selectedInputFramesCount = newValue
					userClient.changeInputFramesCount(UInt16(newValue))
				}
			}
		)
	}
	private var outputFramesBinding: Binding<Int> {
		Binding(
			get: { selectedOutputFramesCount },
			set: { newValue in
				if selectedOutputFramesCount != newValue {
					selectedOutputFramesCount = newValue
					userClient.changeOutputFramesCount(UInt16(newValue))
				}
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
						HStack {
							Picker("URB Count", selection: urbBinding) {
								ForEach(urbCount, id: \.self) { size in
									Text("\(size)")
								}
							}
							.padding()
							.frame(width: 150)
							.pickerStyle(MenuPickerStyle())
							Picker("Input Frames Per Packet", selection: inputFramesBinding) {
								ForEach(inputFramesCount, id: \.self) { size in
									Text("\(size)")
								}
							}
							.padding()
							.frame(width: 150)
							.pickerStyle(MenuPickerStyle())
							Picker("Output Frames Per Packet", selection: outputFramesBinding) {
								ForEach(outputFramesCount, id: \.self) { size in
									Text("\(size)")
								}
							}
							.padding()
							.frame(width: 150)
							.pickerStyle(MenuPickerStyle())
						}
					}
					Text(playbackStatsText)
						.font(.system(.body, design: .monospaced))
						.multilineTextAlignment(.leading)
						.frame(minWidth: 400, maxWidth: 400, minHeight: 200, idealHeight: 200, maxHeight: 200)
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
					selectedInputFramesCount = Int(userClient.getCurrentInputFramesCount())
					selectedOutputFramesCount = Int(userClient.getCurrentOutputFramesCount())
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
		self.playbackStatsText = "Playing             : \(stats.playing)\n" +
			"Recording           : \(stats.recording)\n" +
			"Out Sample Time     : \(stats.out_sample_time)\n" +
			"Out Sample Time USB : \(stats.out_sample_time_usb)\n" +
			"Out Sample Time diff: \(stats.out_sample_time_diff)\n" +
			"In Sample Time      : \(stats.in_sample_time)\n" +
			"In Sample Time USB  : \(stats.in_sample_time_usb)\n" +
			"In Sample Time diff : \(stats.in_sample_time_diff)\n" +
			"XRUNS               : \(stats.xruns)"
	}
}

struct PloytecAppView_Previews: PreviewProvider {
	static var previews: some View {
		PloytecAppView()
	}
}
