import CodexBarCore
import Foundation
import Testing
@testable import CodexBarCLI

private struct NativeUsageFixtureManifest: Decodable {
    let schemaVersion: Int
    let cases: [NativeUsageFixtureCase]
}

private struct NativeUsageFixtureCase: Decodable {
    let name: String
    let payloads: [NativeUsageFixturePayload]
}

private struct NativeUsageFixturePayload: Decodable {
    let provider: String
    let account: String?
    let version: String?
    let source: String
    let status: NativeUsageFixtureStatus?
    let usage: UsageSnapshot?
    let credits: CreditsSnapshot?
    let diagnostic: String?
    let error: NativeUsageFixtureError?
    let pace: NativeUsageFixtureProviderPace?

    func makePayload() throws -> ProviderPayload {
        ProviderPayload(
            providerID: self.provider,
            account: self.account,
            version: self.version,
            source: self.source,
            status: try self.status?.makePayload(),
            usage: self.usage,
            credits: self.credits,
            antigravityPlanInfo: nil,
            openaiDashboard: nil,
            error: try self.error?.makePayload(),
            diagnostic: self.diagnostic,
            pace: self.pace?.makePayload())
    }
}

private struct NativeUsageFixtureStatus: Decodable {
    let indicator: String
    let description: String?
    let updatedAt: Date?
    let url: String

    func makePayload() throws -> ProviderStatusPayload {
        guard let indicator = ProviderStatusPayload.ProviderStatusIndicator(rawValue: self.indicator) else {
            throw NativeUsageFixtureError.invalidValue("status indicator", self.indicator)
        }
        return ProviderStatusPayload(
            indicator: indicator,
            description: self.description,
            updatedAt: self.updatedAt,
            url: self.url)
    }
}

private struct NativeUsageFixtureProviderPace: Decodable {
    let primary: NativeUsageFixturePace?
    let secondary: NativeUsageFixturePace?

    func makePayload() -> ProviderPacePayload {
        ProviderPacePayload(primary: self.primary?.makePayload(), secondary: self.secondary?.makePayload())
    }
}

private struct NativeUsageFixturePace: Decodable {
    let stage: String
    let deltaPercent: Double
    let expectedUsedPercent: Double
    let willLastToReset: Bool
    let etaSeconds: TimeInterval?
    let runOutProbability: Double?
    let summary: String

    func makePayload() -> PacePayload {
        PacePayload(
            stage: self.stage,
            deltaPercent: self.deltaPercent,
            expectedUsedPercent: self.expectedUsedPercent,
            willLastToReset: self.willLastToReset,
            etaSeconds: self.etaSeconds,
            runOutProbability: self.runOutProbability,
            summary: self.summary)
    }
}

private struct NativeUsageFixtureError: Decodable, Error {
    let code: Int32
    let message: String
    let kind: String?

    static func invalidValue(_ field: String, _ value: String) -> Self {
        Self(code: 1, message: "Invalid \(field): \(value)", kind: nil)
    }

    func makePayload() throws -> ProviderErrorPayload {
        let kind = try self.kind.map { value in
            guard let kind = CLIErrorKind(rawValue: value) else {
                throw Self.invalidValue("error kind", value)
            }
            return kind
        }
        return ProviderErrorPayload(code: self.code, message: self.message, kind: kind)
    }
}

struct NativeUsageFixtureTests {
    @Test
    func `shared usage payloads encode identically in swift`() throws {
        let fixtureURL = URL(fileURLWithPath: #filePath)
            .deletingLastPathComponent()
            .deletingLastPathComponent()
            .appendingPathComponent("fixtures/native/usage-serialization-v1.json")
        let data = try Data(contentsOf: fixtureURL)
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let manifest = try decoder.decode(NativeUsageFixtureManifest.self, from: data)
        #expect(manifest.schemaVersion == 1)

        let rawManifest = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
        let rawCases = try #require(rawManifest["cases"] as? [[String: Any]])
        #expect(rawCases.count == manifest.cases.count)

        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        encoder.outputFormatting = [.sortedKeys]
        for (index, testCase) in manifest.cases.enumerated() {
            let actualData = try encoder.encode(try testCase.payloads.map { try $0.makePayload() })
            let actual = try #require(JSONSerialization.jsonObject(with: actualData) as? NSArray)
            let expected = try #require(rawCases[index]["payloads"] as? NSArray)
            #expect(actual.isEqual(expected), "Fixture case \(testCase.name) diverged")
        }
    }
}
