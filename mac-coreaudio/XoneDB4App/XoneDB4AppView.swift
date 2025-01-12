import SwiftUI

struct XoneDB4AppView: View {
	@ObservedObject var viewModel = XoneDB4AppViewModel()
	var userClient = XoneDB4AppUserClient()
	@State private var userClientText = ""
	@State private var firmwareVersionText = ""
	@State private var deviceNameText = ""
	@State private var deviceManufacturerText = ""
	@State private var playbackStatsText = ""
	@State private var selectedBufferSize = 2560
	let bufferSize = [160, 320, 480, 640, 800, 960, 1120, 1280, 1440, 1600, 1760, 1920, 2080, 2240, 2400, 2560, 2720, 2880, 3040, 3200, 3360, 3520, 3680, 3840, 4000]
	private let playbackStatsUpdateInterval = 1.0
	@State private var timer: Timer?

	var body: some View {
		ScrollView {
			VStack {
				// Driver Manager Section
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
				
				// Spacer to push content to top
				Spacer()
				
				// User Client Manager Section
				VStack(alignment: .center) {
					Text("User Client Manager")
						.padding()
						.font(.title)
					Text(userClientText)
						.multilineTextAlignment(.center)
					HStack {
						Button(action: {
							userClientText = self.userClient.openConnection()
							firmwareVersionText = self.userClient.getFirmwareVersion()
							deviceNameText = self.userClient.getDeviceName()
							deviceManufacturerText = self.userClient.getDeviceManufacturer()
						}, label: {
							Text("Open User Client")
						})
						Spacer()
						// Add a Picker for the dropdown menu
						Picker("Buffersize", selection: $selectedBufferSize) {
							ForEach(bufferSize, id: \.self) { size in
								Text("\(size)")
							}
						}
						.padding()
						.frame(width: 200)
						.pickerStyle(MenuPickerStyle()) // Display as dropdown menu
						.onChange(of: selectedBufferSize) { newValue in
							self.userClient.changeBufferSize(UInt32(newValue))
						}
					}
					.padding()

					Text(deviceNameText)
					Text(deviceManufacturerText)
					Text(firmwareVersionText)
					
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
					startTimer()
				} else {
					stopTimer()
				}
			}
		}
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

struct XoneDB4AppView_Previews: PreviewProvider {
	static var previews: some View {
		XoneDB4AppView()
	}
}
