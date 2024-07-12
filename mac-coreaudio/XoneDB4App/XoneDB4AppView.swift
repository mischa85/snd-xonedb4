import SwiftUI

struct XoneDB4AppView: View {
	@ObservedObject var viewModel = XoneDB4AppViewModel()
	var userClient = XoneDB4AppUserClient()
	@State private var userClientText = ""
	@State private var firmwareVersionText = ""
	@State private var playbackStatsText = ""
	@State private var selectedBufferSize = 800
	let bufferSize = [40, 80, 120, 160, 200, 240, 280, 320, 360, 400, 440, 480, 520, 560, 600, 640, 680, 720, 760, 800, 840, 880, 920, 960, 1000]
	private let playbackStatsUpdateInterval = 0.1
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
					
					Text(firmwareVersionText)
						.padding()
					
					Text(playbackStatsText)
						.font(.system(.body, design: .monospaced))
						.multilineTextAlignment(.leading)
						.frame(minWidth: 300, maxWidth: 300, minHeight: 100, idealHeight: 100, maxHeight: 200)
						.border(Color.gray)
						.padding()
				}
				.frame(width: 500, alignment: .center)
			}
		}
		.onAppear {
			// Start timer to update playback stats
			self.timer = Timer.scheduledTimer(withTimeInterval: self.playbackStatsUpdateInterval, repeats: true) { _ in
				self.updatePlaybackStats()
			}
		}
		.onDisappear {
			// Stop timer when view disappears
			self.timer?.invalidate()
			self.timer = nil
		}
	}
	
	private func updatePlaybackStats() {
		let stats = self.userClient.getPlaybackStats()
		let outdiff = stats.out_sample_time_usb - stats.out_sample_time
		let indiff = stats.in_sample_time_usb - stats.in_sample_time
		self.playbackStatsText = "Out Sample Time     : \(stats.out_sample_time)\n" +
			"Out Sample Time USB : \(stats.out_sample_time_usb)\n" +
			"In Sample Time      : \(stats.in_sample_time)\n" +
			"In Sample Time  USB : \(stats.in_sample_time_usb)"
	}
}

struct XoneDB4AppView_Previews: PreviewProvider {
	static var previews: some View {
		XoneDB4AppView()
	}
}
