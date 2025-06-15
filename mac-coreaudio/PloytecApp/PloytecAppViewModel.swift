import Foundation
import Combine
import os.log
#if os(macOS)
import SystemExtensions
#endif

class PloytecAppStateMachine {

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

class PloytecAppViewModel: NSObject {
	
	private let dextIdentifier: String = "sc.hackerman.ploytecdriver"
	private let midiBridge = PloytecAppUserClientSwift()
	
	// Check the initial state of the dext because it doesn't necessarily start in an unloaded state.
	@Published private(set) var state: PloytecAppStateMachine.State = .deactivated
	@Published var isConnected = false
	
	private var cancellables = Set<AnyCancellable>()
	
	override init() {
		super.init()
		NotificationCenter.default.publisher(for: NSNotification.Name("UserClientConnectionOpened"))
			.sink { [weak self] _ in
				self?.isConnected = true
			}
			.store(in: &cancellables)
	}
	
	public var dextLoadingState: String {
		switch state {
		case .activating:
			return "Activating PloytecDriver, please wait."
		case .needsActivatingApproval:
			return "Please follow the prompt to approve PloytecDriver."
		case .needsDeactivatingApproval:
			return "Please follow the prompt to remove PloytecDriver."
		case .activated:
			return "PloytecDriver has been activated and is ready to use."
		case .activationError:
			return "PloytecDriver has experienced an error during activation.\nPlease check the logs to find the error."
		case .deactivationError:
			return "PloytecDriver has experienced an error during deactivation.\nPlease check the logs to find the error."
		case .deactivating:
			return "Deactivating PloytecDriver, please wait."
		case .deactivated:
			return "PloytecDriver deactivated."
		case .dextNotPresentError:
			return "Error: dext is not present."
		case .codeSigningError:
			return "Error: code signing.\nMake sure SIP is disabled (csrutil disable in recovery)\nand amfi_get_out_of_my_way=0x1 is added to the bootflags."
		}
	}
}
	
extension PloytecAppViewModel: ObservableObject {

}
	
extension PloytecAppViewModel {
#if os(macOS)
	func activateMyDext() {
		activateExtension(dextIdentifier)
	}
	
	func deactivateMyDext() {
		deactivateExtension(dextIdentifier)
	}
	
	func activateExtension(_ dextIdentifier: String) {
		let request = OSSystemExtensionRequest.activationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)

		self.state = PloytecAppStateMachine.process(self.state, .activationStarted)
	}

	func deactivateExtension(_ dextIdentifier: String) {
		let request = OSSystemExtensionRequest.deactivationRequest(forExtensionWithIdentifier: dextIdentifier, queue: .main)
		request.delegate = self
		OSSystemExtensionManager.shared.submitRequest(request)
		
		self.state = PloytecAppStateMachine.process(self.state, .deactivationStarted)
	}
#endif
}

#if os(macOS)
extension PloytecAppViewModel: OSSystemExtensionRequestDelegate {
	
	func request(
		_ request: OSSystemExtensionRequest,
		actionForReplacingExtension existing: OSSystemExtensionProperties,
		withExtension ext: OSSystemExtensionProperties) -> OSSystemExtensionRequest.ReplacementAction {

		var replacementAction: OSSystemExtensionRequest.ReplacementAction

		os_log("system extension actionForReplacingExtension: %@ %@", existing, ext)

		// Add appropriate logic here to determine whether to replace the extension
		// with the new extension. Common things to check for include
		// testing whether the new extension's version number is newer than
		// the current version number and whether the bundleIdentifier is different.
		// For simplicity, this sample always replaces the current extension
		// with the new one.
		replacementAction = .replace

		// The upgrade case may require a separate set of states.
		self.state = PloytecAppStateMachine.process(self.state, .activationStarted)

		return replacementAction
	}

	func requestNeedsUserApproval(_ request: OSSystemExtensionRequest) {
		os_log("system extension requestNeedsUserApproval")
		self.state = PloytecAppStateMachine.process(self.state, .promptForApproval)
	}

	func request(_ request: OSSystemExtensionRequest, didFinishWithResult result: OSSystemExtensionRequest.Result) {
		os_log("system extension didFinishWithResult: %d", result.rawValue)
		self.state = PloytecAppStateMachine.process(self.state, .activationFinished)
	}

	func request(_ request: OSSystemExtensionRequest, didFailWithError error: Error) {
		os_log("system extension didFailWithError: %@", error.localizedDescription)
		if let extensionError = error as NSError?, extensionError.domain == OSSystemExtensionErrorDomain {
			if extensionError.code == 4 {
				self.state = PloytecAppStateMachine.process(self.state, .dextNotPresent)
			} else if extensionError.code == 8 {
				self.state = PloytecAppStateMachine.process(self.state, .codeSigningErr)
			}
		}
		self.state = PloytecAppStateMachine.process(self.state, .activationFailed)
	}
}
#endif
