import Foundation
#if canImport(FoundationNetworking)
import FoundationNetworking
#endif
import Testing
@testable import CodexBarCLI
@testable import CodexBarCore

struct AzureOpenAIProviderLinuxTests {
    @Test
    func `builds legacy request and maps deployment identity`() async throws {
        let transport = ProviderHTTPTransportHandler { request in
            #expect(request.url?.absoluteString ==
                "https://resource.openai.azure.com/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21")
            #expect(request.httpMethod == "POST")
            #expect(request.value(forHTTPHeaderField: "api-key") == "key")
            #expect(request.value(forHTTPHeaderField: "Accept") == "application/json")
            #expect(request.value(forHTTPHeaderField: "Content-Type") == "application/json")
            #expect(request.timeoutInterval == 20)
            let body = try #require(request.httpBody)
            let payload = try #require(JSONSerialization.jsonObject(with: body) as? [String: Any])
            #expect(payload["max_tokens"] as? Int == 1)
            #expect(payload["max_completion_tokens"] == nil)
            #expect(payload["model"] == nil)
            return try Self.response(request, #"{"id":"cmpl-1","model":"gpt-4o-mini"}"#)
        }

        let snapshot = try await AzureOpenAIUsageFetcher.fetchUsage(
            apiKey: " key ",
            endpoint: #require(URL(string: "https://resource.openai.azure.com")),
            deploymentName: " chat-prod ",
            transport: transport,
            updatedAt: Date(timeIntervalSince1970: 1_800_000_000))
        let usage = snapshot.toUsageSnapshot()

        #expect(snapshot.apiVersion == "2024-10-21")
        #expect(usage.primary?.usedPercent == 0)
        #expect(usage.primary?.resetDescription == "Deployment: chat-prod · Model: gpt-4o-mini")
        #expect(usage.secondary == nil)
        #expect(usage.tertiary == nil)
        #expect(usage.accountOrganization(for: .azureopenai) == "resource.openai.azure.com")
        #expect(usage.loginMethod(for: .azureopenai) == "Deployment: chat-prod")
        #expect(usage.dataConfidence == .unknown)
    }

    @Test
    func `builds v1 request without duplicating endpoint suffix`() async throws {
        let transport = ProviderHTTPTransportHandler { request in
            #expect(request.url?.absoluteString == "https://proxy.example.com/base/openai/v1/chat/completions")
            let body = try #require(request.httpBody)
            let payload = try #require(JSONSerialization.jsonObject(with: body) as? [String: Any])
            #expect(payload["model"] as? String == "chat-prod")
            #expect(payload["max_completion_tokens"] as? Int == 1)
            #expect(payload["max_tokens"] == nil)
            return try Self.response(request, #"{"model":null}"#)
        }

        let snapshot = try await AzureOpenAIUsageFetcher.fetchUsage(
            apiKey: "key",
            endpoint: #require(URL(string: "https://proxy.example.com/base/openai/v1")),
            deploymentName: "chat-prod",
            apiVersion: " V1 ",
            transport: transport,
            updatedAt: Date(timeIntervalSince1970: 1))

        #expect(snapshot.apiVersion == "V1")
        #expect(snapshot.toUsageSnapshot().primary?.resetDescription == "Deployment: chat-prod")
    }

    @Test
    func `escapes deployment and preserves path overlap`() throws {
        let escaped = try AzureOpenAIUsageFetcher._chatCompletionsURLForTesting(
            endpoint: #require(URL(string: "https://proxy.example.com/base")),
            deploymentName: "chat prod",
            apiVersion: "2024 10")
        #expect(escaped.absoluteString ==
            "https://proxy.example.com/base/openai/deployments/chat%20prod/chat/completions?api-version=2024%2010")

        let overlapping = try AzureOpenAIUsageFetcher._chatCompletionsURLForTesting(
            endpoint: #require(URL(string: "https://proxy.example.com/base/openai")),
            deploymentName: "chat-prod",
            apiVersion: "2024-10-21")
        #expect(overlapping.absoluteString ==
            "https://proxy.example.com/base/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21")

        let encodedPath = try AzureOpenAIUsageFetcher._chatCompletionsURLForTesting(
            endpoint: #require(URL(string: "https://proxy.example.com/base%2Ftenant")),
            deploymentName: "chat-prod",
            apiVersion: "2024-10-21")
        #expect(encodedPath.absoluteString ==
            "https://proxy.example.com/base%2Ftenant/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21")

        let encodedQuery = try AzureOpenAIUsageFetcher._chatCompletionsURLForTesting(
            endpoint: #require(URL(string: "https://proxy.example.com/base/openai/v1?sig=a%26b#frag%2Fment")),
            deploymentName: "chat-prod",
            apiVersion: "v1")
        #expect(encodedQuery.absoluteString ==
            "https://proxy.example.com/base/openai/v1/chat/completions?sig=a%26b#frag%2Fment")
    }

    @Test
    func `does not retry post failures and rejects malformed success`() async throws {
        let failing = AzureOpenAISequenceTransport(results: [
            .response(status: 429, body: "  too\n  many requests  "),
            .response(status: 200, body: "{}"),
        ])
        await #expect {
            _ = try await AzureOpenAIUsageFetcher.fetchUsage(
                apiKey: "key",
                endpoint: #require(URL(string: "https://resource.openai.azure.com")),
                deploymentName: "chat-prod",
                transport: failing)
        } throws: { error in
            guard case let AzureOpenAIUsageError.apiError(status, message) = error else { return false }
            return status == 429 && message == "too many requests"
        }
        #expect(await failing.requestCount == 1)

        let malformed = ProviderHTTPTransportHandler { request in
            try Self.response(request, #"{"model":1}"#)
        }
        await #expect {
            _ = try await AzureOpenAIUsageFetcher.fetchUsage(
                apiKey: "key",
                endpoint: #require(URL(string: "https://resource.openai.azure.com")),
                deploymentName: "chat-prod",
                transport: malformed)
        } throws: { error in
            guard case AzureOpenAIUsageError.parseFailed = error else { return false }
            return true
        }
    }

    @Test
    func `applies config overrides and resolves aliases`() {
        let config = ProviderConfig(
            id: .azureopenai,
            apiKey: "config-key",
            workspaceID: "chat-prod",
            enterpriseHost: "https://resource.openai.azure.com")
        let environment = ProviderConfigEnvironment.applyProviderConfigOverrides(
            base: [
                AzureOpenAISettingsReader.apiKeyEnvironmentKey: "environment-key",
                AzureOpenAISettingsReader.endpointEnvironmentKey: "https://environment.example.com",
                AzureOpenAISettingsReader.deploymentNameEnvironmentKey: "environment-deployment",
            ],
            provider: .azureopenai,
            config: config)

        #expect(AzureOpenAISettingsReader.apiKey(environment: environment) == "config-key")
        #expect(AzureOpenAISettingsReader.endpoint(environment: environment)?.absoluteString ==
            "https://resource.openai.azure.com")
        #expect(AzureOpenAISettingsReader.deploymentName(environment: environment) == "chat-prod")
        #expect(ProviderSelection(argument: "aoai")?.asList == [.azureopenai])
        #expect(ProviderSelection(argument: "azure-openai")?.asList == [.azureopenai])
    }

    fileprivate static func response(_ request: URLRequest, _ body: String, status: Int = 200) throws
        -> (Data, URLResponse)
    {
        guard let url = request.url,
              let response = HTTPURLResponse(
                  url: url,
                  statusCode: status,
                  httpVersion: "HTTP/1.1",
                  headerFields: ["Content-Type": "application/json"])
        else { throw URLError(.badServerResponse) }
        return (Data(body.utf8), response)
    }
}

private actor AzureOpenAISequenceTransport: ProviderHTTPTransport {
    enum Result {
        case response(status: Int, body: String)
    }

    private var results: [Result]
    private(set) var requestCount = 0

    init(results: [Result]) {
        self.results = results
    }

    func data(for request: URLRequest) async throws -> (Data, URLResponse) {
        self.requestCount += 1
        switch self.results.removeFirst() {
        case let .response(status, body):
            return try AzureOpenAIProviderLinuxTests.response(request, body, status: status)
        }
    }
}
