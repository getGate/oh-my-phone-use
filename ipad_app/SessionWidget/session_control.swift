import AppIntents
import SwiftUI
import WidgetKit

@available(iOS 18.0, *)
struct SessionControl: ControlWidget {
    var body: some ControlWidgetConfiguration {
        StaticControlConfiguration(kind: "ipad_computer_use.start_session") {
            ControlWidgetButton(action: StartSessionIntent()) {
                Label("Start Session", systemImage: "cursorarrow.motionlines")
            }
        }
        .displayName("Oh My Phone Use")
        .description("Start a session and confirm screen sharing.")
    }
}
