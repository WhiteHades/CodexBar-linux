import Foundation
import Testing
@testable import CodexBarCore

struct CodexBarConfigUnknownProviderTests {
    @Test
    func `unknown provider entries do not invalidate persisted config`() throws {
        let data = Data(#"""
        {
          "version": 1,
          "providers": [
            {"id": "kimik2", "enabled": true},
            {"id": "crossmodel", "enabled": true},
            {"id": "future-provider", "enabled": "malformed but ignored"},
            {"id": "codex", "enabled": false, "source": "oauth"}
          ]
        }
        """#.utf8)

        let decoded = try JSONDecoder().decode(CodexBarConfig.self, from: data)

        #expect(decoded.providers.map(\.id) == [.codex])
        #expect(decoded.providerConfig(for: .codex)?.enabled == false)
        #expect(decoded.providerConfig(for: .codex)?.source == .oauth)
    }

    @Test
    func `malformed provider identifiers still invalidate persisted config`() {
        let malformedEntries = [
            #"{"enabled":true}"#,
            #"{"id":42,"enabled":true}"#,
        ]

        for entry in malformedEntries {
            let data = Data(#"{"version":1,"providers":[\#(entry)]}"#.utf8)
            #expect(throws: DecodingError.self) {
                try JSONDecoder().decode(CodexBarConfig.self, from: data)
            }
        }
    }

    @Test
    func `malformed known provider entries still invalidate persisted config`() {
        let data = Data(#"{"version":1,"providers":[{"id":"codex","enabled":"yes"}]}"#.utf8)

        #expect(throws: DecodingError.self) {
            try JSONDecoder().decode(CodexBarConfig.self, from: data)
        }
    }
}
