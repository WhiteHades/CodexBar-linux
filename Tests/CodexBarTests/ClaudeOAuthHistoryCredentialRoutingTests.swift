import Foundation
import Testing
@testable import CodexBarCore

@Suite(.serialized)
struct ClaudeOAuthHistoryCredentialRoutingTests {
    @Test
    func `history keychain reference only matches the credential that won routing`() throws {
        let keychainData = self.makeCredentialsData(accessToken: "keychain-token")
        let keychainCredentials = try ClaudeOAuthCredentials.parse(data: keychainData)
        let differentCredentials = try ClaudeOAuthCredentials.parse(
            data: self.makeCredentialsData(accessToken: "different-token"))
        let fingerprint = ClaudeOAuthCredentialsStore.ClaudeKeychainFingerprint(
            modifiedAt: 1,
            createdAt: 1,
            persistentRefHash: "opaque-ref")

        let matchingCLIRecord = ClaudeOAuthCredentialRecord(
            credentials: keychainCredentials,
            owner: .claudeCLI,
            source: .memoryCache)
        let differentCLIRecord = ClaudeOAuthCredentialRecord(
            credentials: differentCredentials,
            owner: .claudeCLI,
            source: .credentialsFile)
        let matchingEnvironmentRecord = ClaudeOAuthCredentialRecord(
            credentials: keychainCredentials,
            owner: .environment,
            source: .environment)
        let matchingCodexBarRecord = ClaudeOAuthCredentialRecord(
            credentials: keychainCredentials,
            owner: .codexbar,
            source: .cacheKeychain)

        ProviderInteractionContext.$current.withValue(.userInitiated) {
            ClaudeOAuthKeychainPromptPreference.withTaskOverrideForTesting(.always) {
                ClaudeOAuthCredentialsStore.withClaudeKeychainOverridesForTesting(
                    data: keychainData,
                    fingerprint: fingerprint)
                {
                    #expect(ClaudeOAuthCredentialsStore
                        .matchingClaudeKeychainPersistentRefHashWithoutPrompt(for: matchingCLIRecord) == "opaque-ref")
                    #expect(ClaudeOAuthCredentialsStore
                        .matchingClaudeKeychainPersistentRefHashWithoutPrompt(for: differentCLIRecord) == nil)
                    #expect(ClaudeOAuthCredentialsStore
                        .matchingClaudeKeychainPersistentRefHashWithoutPrompt(for: matchingEnvironmentRecord) == nil)
                    #expect(ClaudeOAuthCredentialsStore
                        .matchingClaudeKeychainPersistentRefHashWithoutPrompt(for: matchingCodexBarRecord) == nil)
                }
            }
        }
    }

    private func makeCredentialsData(accessToken: String) -> Data {
        let expiresAt = Int(Date(timeIntervalSinceNow: 3600).timeIntervalSince1970 * 1000)
        return Data("""
        {
          "claudeAiOauth": {
            "accessToken": "\(accessToken)",
            "expiresAt": \(expiresAt),
            "scopes": ["user:profile"]
          }
        }
        """.utf8)
    }
}
