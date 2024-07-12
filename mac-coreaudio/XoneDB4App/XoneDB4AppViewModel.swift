//
//  XoneDB4AppViewModel.swift
//  XoneDB4App
//
//  Created by Marcel Bierling on 04/07/2024.
//  Copyright © 2024 Hackerman. All rights reserved.
//

import Foundation
import os.log
#if os(macOS)
import SystemExtensions
#endif

class XoneDB4AppStateMachine {

	enum State {
		case activating
		case deactivating
		case needsActivatingApproval
		case needsDeactivatingApproval
		case activated
		case deactivated
		case activationError
		case deactivationError
		case dextNotPresentError
		case codeSigningError
	}

	enum Event {
		case activationStarted
		case deactivationStarted
		case promptForApproval
		case activationFinished
		case deactivationFinished
		case activationFailed
		case deactivationFailed
		case dextNotPresent
		case codeSigningErr
	}

	static func onActivatingOrNeedsApproval(_ event: Event) -> State {
		switch event {
		case .activationStarted:
			return .activating
		case .promptForApproval:
			return .needsActivatingApproval
		case .activationFinished:
			return .activated
		case .activationFailed, .deactivationStarted, .deactivationFinished, .deactivationFailed:
			return .activationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}

	static func onActivated(_ event: Event) -> State {
		switch event {
		case .activationStarted:
			return .activating
		case .activationFinished:
			return .activated
		case .deactivationStarted:
			return .deactivating
		case .promptForApproval, .activationFailed, .deactivationFailed, .deactivationFinished:
			return .activationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}

	static func onActivationError(_ event: Event) -> State {
		switch event {
		case .activationStarted:
			return .activating
		case .promptForApproval, .activationFinished, .activationFailed, .deactivationStarted, .deactivationFinished, .deactivationFailed:
			return .activationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}
	
	static func onDeactivatingOrNeedsApproval(_ event: Event) -> State {
		switch event {
		case .deactivationStarted:
			return .deactivating
		case .promptForApproval:
			return .needsDeactivatingApproval
		case .deactivationFinished:
			return .deactivated
		case .deactivationFailed, .activationStarted, .activationFinished, .activationFailed:
			return .deactivationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}
	
	static func onDeActivated(_ event: Event) -> State {
		switch event {
		case .activationStarted:
			return .activating
		case .deactivationStarted:
			return .deactivating
		case .deactivationFinished:
			return .deactivated
		case .promptForApproval, .activationFinished, .activationFailed, .deactivationFailed:
			return .deactivationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}

	static func onDeactivationError(_ event: Event) -> State {
		switch event {
		case .deactivationStarted:
			return .deactivating
		case .activationStarted:
			return .activating
		case .promptForApproval, .deactivationFinished, .deactivationFailed, .activationFinished, .activationFailed:
			return .deactivationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}
	
	static func onDextNotPresentError(_ event: Event) -> State {
		switch event {
		case .deactivationStarted:
			return .deactivating
		case .activationStarted:
			return .activating
		case .promptForApproval, .deactivationFinished, .deactivationFailed, .activationFinished, .activationFailed:
			return .deactivationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}
	
	static func onCodeSigningError(_ event: Event) -> State {
		switch event {
		case .deactivationStarted:
			return .deactivating
		case .activationStarted:
			return .activating
		case .promptForApproval, .deactivationFinished, .deactivationFailed, .activationFinished, .activationFailed:
			return .deactivationError
		case .dextNotPresent:
			return .dextNotPresentError
		case .codeSigningErr:
			return .codeSigningError
		}
	}

	static func process(_ state: State, _ event: Event) -> State {

		switch state {
		case .deactivated:
			return onDeActivated(event)
		case .activating, .needsActivatingApproval:
			return onActivatingOrNeedsApproval(event)
		case .activated:
			return onActivated(event)
		case .activationError:
			return onActivationError(event)
		case .deactivating, .needsDeactivatingApproval:
			return onDeactivatingOrNeedsApproval(event)
		case .deactivationError:
			return onDeactivationError(event)
		case .dextNotPresentError:
			return onDextNotPresentError(event)
		case .codeSigningError:
			return onCodeSigningError(event)
		}
	}
}

class XoneDB4AppViewModel: NSObject, ObservableObject {

	private let dextIdentifier: String = "sc.hackerman.xonedb4driver"
	
	// Check the initial state of the dext because it doesn't necessarily start in an unloaded state.
	@Published private(set) var state: XoneDB4AppStateMachine.State = .deactivated
	
	override init() {
		super.init()
		checkactivatedstateMyDext()
	}

	public var dextLoadingState: String {
		switch state {
		case .activating:
			return "Activating XoneDB4Driver, please wait."
		case .needsActivatingApproval:
			return "Please follow the prompt to approve XoneDB4Driver."
		case .needsDeactivatingApproval:
			return "Please follow the prompt to remove XoneDB4Driver."
		case .activated:
			return "XoneDB4Driver has been activated and is ready to use."
		case .activationError:
			return "XoneDB4Driver has experienced an error during activation.\nPlease check the logs to find the error."
		case .deactivationError:
			return "XoneDB4Driver has experienced an error during deactivation.\nPlease check the logs to find the error."
		case .deactivating:
			return "Deactivating XoneDB4Driver, please wait."
		case .deactivated:
			return "XoneDB4Driver deactivated."
		case .dextNotPresentError:
			return "Error: dext is not present."
		case .codeSigningError:
			return "Error: code signing.\nMake sure SIP is disabled (csrutil disable in recovery)\nand amfi_get_out_of_my_way=0x1 is added to the bootflags."
		}
	}
	
	func checkactivatedstateMyDext() {
		checkExtension(dextIdentifier)
	}
	
	func activateMyDext() {
		activateExtension(dextIdentifier)
	}
	
	func deactivateMyDext() {
		deactivateExtension(dextIdentifier)
	}

	func checkExtension(_ dextIdentifier: String) {
		let request = OSSystemExtensionRequest.propertiesRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)
	}
	
	func activateExtension(_ dextIdentifier: String) {
		let request = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)

		self.state = XoneDB4AppStateMachine.process(self.state, .activationStarted)
	}

	func deactivateExtension(_ dextIdentifier: String) {
		let request = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)
		
		self.state = XoneDB4AppStateMachine.process(self.state, .deactivationStarted)
	}
}

#if os(macOS)
extension XoneDB4AppViewModel: OSSystemExtensionRequestDelegate {
	
	func request(_ request: OSSystemExtensionRequest, foundProperties properties: OSSystemExtensionProperties) {
		if properties.isEnabled {
			NSLog("FOUND EXT ACTIVATED")
			self.state = .activated
		} else {
			NSLog("FOUND EXT DEACTIVATED")
			self.state = .deactivated
		}
	}
	
	func request(_ request: OSSystemExtensionRequest, actionForReplacingExtension existing: OSSystemExtensionProperties, withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {
		os_log("system extension actionForReplacingExtension: %@ %@", existing, ext)
		return .replace
	}

	func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
		os_log("system extension requestNeedsUserApproval")
		self.state = XoneDB4AppStateMachine.process(self.state, .promptForApproval)
	}

	func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
		os_log("system extension didFinishWithResult: %d", result.rawValue)
		self.state = XoneDB4AppStateMachine.process(self.state, .activationFinished)
	}

	func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
		os_log("system extension didFailWithError: %@", error.localizedDescription)
		if let extensionError = error as NSError?, extensionError.domain == OSSystemExtensionErrorDomain {
			if extensionError.code == 4 {
				self.state = XoneDB4AppStateMachine.process(self.state, .dextNotPresent)
			} else if extensionError.code == 8 {
				self.state = XoneDB4AppStateMachine.process(self.state, .codeSigningErr)
			}
		}
		self.state = XoneDB4AppStateMachine.process(self.state, .activationFailed)
	}
}
#endif
