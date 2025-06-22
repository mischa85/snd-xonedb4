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
	private let urbCount = [1, 2, 3, 4, 5, 6, 7, 8]

	@State private var selectedUrbCount = 4
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
					Text(deviceNameText)
					Text(deviceManufacturerText)
					Text(firmwareVersionText)
					if viewModel.isConnected {
						Picker("URB Count", selection: urbBinding) {
							ForEach(urbCount, id: \.self) { size in
								Text("\(size)")
							}
						}
						.padding()
						.frame(width: 150)
						.pickerStyle(MenuPickerStyle())
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
