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
					Button(action: {
						userClientText = self.userClient.openConnection()
						firmwareVersionText = self.userClient.getFirmwareVersion()
						deviceNameText = self.userClient.getDeviceName()
						deviceManufacturerText = self.userClient.getDeviceManufacturer()
					}, label: {
						Text("Open User Client")
					})
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

struct PloytecAppView_Previews: PreviewProvider {
	static var previews: some View {
		PloytecAppView()
	}
}
