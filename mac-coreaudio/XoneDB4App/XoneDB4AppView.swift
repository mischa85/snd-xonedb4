//
//  ContentView.swift
//  XoneDB4Driver
//
//  Created by Marcel Bierling on 01/06/24.
//  Copyright © 2024 Hackerman. All rights reserved.
//

import SwiftUI

struct XoneDB4AppView: View {
	@ObservedObject var viewModel = XoneDB4AppViewModel()
	var userClient = XoneDB4AppUserClient()
	@State var userClientText = ""
	@State var firmwareversionText = ""

	var body: some View {
#if os(macOS)
		VStack(alignment: .center) {
			Text("Driver Manager")
				.padding()
				.font(.title)
			Text(self.viewModel.dextLoadingState)
				.multilineTextAlignment(.center)
			HStack {
				Button(
					action: {
						self.viewModel.activateMyDext()
					}, label: {
						Text("Install Dext")
					}
				)
				Button(
					action: {
						self.viewModel.deactivateMyDext()
					}, label: {
						Text("Uninstall Dext")
					}
				)
			}
		}
		.frame(width: 500, height: 200, alignment: .center)
#endif
		VStack(alignment: .center) {
			Text("User Client Manager")
				.padding()
				.font(.title)
			Text(userClientText)
				.multilineTextAlignment(.center)
			HStack {
				Button(
					action: {
						userClientText = self.userClient.openConnection()
					}, label: {
						Text("Open User Client")
					}
				)
				Spacer()
				Button(
					action: {
						userClientText = self.userClient.getFirmwareVersion()
					}, label: {
						Text("Get Firmware")
					}
				)
			}
			.padding()
			Text(firmwareversionText)
		}
		.frame(width: 500, height: 200, alignment: .center)
	}
}

struct XoneDB4AppView_Previews: PreviewProvider {
	static var previews: some View {
		XoneDB4AppView()
	}
}
