import SwiftUI

struct OzzyView: View {
	@ObservedObject var viewModel = OzzyViewModel()
	var userClient = OzzyUserClient()
	@State private var userClientText = ""
	@State private var firmwareVersionText = ""
	@State private var deviceNameText = ""
	@State private var deviceManufacturerText = ""

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
							//firmwareVersionText = self.userClient.getFirmwareVersion()
							//deviceNameText = self.userClient.getDeviceName()
							//deviceManufacturerText = self.userClient.getDeviceManufacturer()
						}, label: {
							Text("Open User Client")
						})
						Spacer()
					}
					.padding()

					Text(deviceNameText)
					Text(deviceManufacturerText)
					Text(firmwareVersionText)
				}
				.frame(width: 500, alignment: .center)
			}
			Text(viewModel.isConnected ? "Connected" : "Not Connected")
				.font(.headline)
				.padding()
				.foregroundColor(viewModel.isConnected ? .green : .red)
		}
	}
}

struct OzzyView_Previews: PreviewProvider {
	static var previews: some View {
		OzzyView()
	}
}
